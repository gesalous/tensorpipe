// Two-process (and so two-host) echo test of tensorpipe's ibv transport running on the
// Portals shim. Run with TP_IBV_LIBRARY set:
//   server (host A): xhost_echo server <addr-file> [ip]
//   client (host B): xhost_echo client <addr-file>
// The server publishes its listener address in <addr-file> (an NFS path), the client
// waits for it. Phase 1 echoes messages of assorted sizes (one wraps the 2 MiB ring);
// phase 2 pipelines a burst of small messages to stress ordering across the network.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include <tensorpipe/transport/connection.h>
#include <tensorpipe/transport/ibv/factory.h>
#include <tensorpipe/transport/listener.h>

using namespace tensorpipe;
using Clock = std::chrono::steady_clock;

static uint8_t pat(size_t msg, size_t k) { return (uint8_t)(msg * 131 + k * 7 + (k >> 8)); }
static void fill(std::vector<uint8_t>& b, size_t msg) { for (size_t k = 0; k < b.size(); k++) b[k] = pat(msg, k); }
static bool check(const uint8_t* b, size_t n, size_t msg) {
  for (size_t k = 0; k < n; k++) if (b[k] != pat(msg, k)) { std::fprintf(stderr, "msg %zu: byte %zu is %u, want %u\n", msg, k, b[k], pat(msg, k)); return false; }
  return true;
}
#define CHECK_ERR(e, msg) do { if (e) { std::fprintf(stderr, "%s: %s\n", msg, (e).what().c_str()); std::exit(2); } } while (0)

static void rd(transport::Connection& c, std::vector<uint8_t>& b) {
  std::promise<void> p;
  c.read(b.data(), b.size(), [&](const Error& e, const void*, size_t) { CHECK_ERR(e, "read"); p.set_value(); });
  p.get_future().wait();
}
static std::future<void> wr(transport::Connection& c, const std::vector<uint8_t>& b) {
  auto p = std::make_shared<std::promise<void>>();
  c.write(b.data(), b.size(), [p](const Error& e) { CHECK_ERR(e, "write"); p->set_value(); });
  return p->get_future();
}

static const std::vector<size_t> kSizes = {1, 8, 100, 4096, 65536, 1000000, 3 * 1024 * 1024 + 17, 2 * 1024 * 1024, 1};
static const size_t kBurst = 20000, kBurstSize = 64;

int main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "usage: %s server|client <addr-file> [ip]\n", argv[0]); return 1; }
  bool server = !std::strcmp(argv[1], "server");
  auto ctx = transport::ibv::create();
  if (!ctx->isViable()) { std::fprintf(stderr, "ibv transport is not viable (TP_IBV_LIBRARY set?)\n"); return 1; }

  std::shared_ptr<transport::Connection> conn;
  if (server) {
    auto l = ctx->listen(argc > 3 ? argv[3] : "0.0.0.0");
    std::string addr = l->addr();
    { std::ofstream f(std::string(argv[2]) + ".tmp"); f << addr; }
    std::rename((std::string(argv[2]) + ".tmp").c_str(), argv[2]);
    std::printf("listening on %s\n", addr.c_str());
    std::promise<std::shared_ptr<transport::Connection>> p;
    l->accept([&](const Error& e, std::shared_ptr<transport::Connection> c) { CHECK_ERR(e, "accept"); p.set_value(c); });
    conn = p.get_future().get();
  } else {
    std::string addr;
    for (int i = 0; i < 600 && addr.empty(); i++) { std::ifstream f(argv[2]); std::getline(f, addr); if (addr.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
    if (addr.empty()) { std::fprintf(stderr, "no address file\n"); return 1; }
    std::printf("connecting to %s\n", addr.c_str());
    conn = ctx->connect(addr);
  }

  int bad = 0;
  for (size_t i = 0; i < kSizes.size(); i++) {
    std::vector<uint8_t> b(kSizes[i]);
    if (server) {
      rd(*conn, b);
      if (!check(b.data(), b.size(), i)) bad++;
      wr(*conn, b).wait();
    } else {
      fill(b, i);
      auto t0 = Clock::now();
      auto w = wr(*conn, b);
      std::vector<uint8_t> back(b.size());
      rd(*conn, back);
      w.wait();
      double us = std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
      bool ok = check(back.data(), back.size(), i);
      if (!ok) bad++;
      std::printf("echo %9zu B: %s  %.0f us\n", b.size(), ok ? "ok" : "CORRUPT", us);
    }
  }

  // Phase 2: kBurst small messages, all written before any is read back.
  if (server) {
    for (size_t i = 0; i < kBurst; i++) {
      std::vector<uint8_t> b(kBurstSize);
      rd(*conn, b);
      if (!check(b.data(), b.size(), 1000 + i)) { if (++bad > 5) break; }
    }
    std::vector<uint8_t> done(1, 0xD0);
    wr(*conn, done).wait();
    std::vector<uint8_t> bye(1);
    rd(*conn, bye); // do not close before the client has seen everything
  } else {
    auto t0 = Clock::now();
    std::vector<std::vector<uint8_t>> msgs(kBurst, std::vector<uint8_t>(kBurstSize));
    std::vector<std::future<void>> fs;
    for (size_t i = 0; i < kBurst; i++) { fill(msgs[i], 1000 + i); fs.push_back(wr(*conn, msgs[i])); }
    std::vector<uint8_t> done(1);
    rd(*conn, done);
    for (auto& f : fs) f.wait();
    std::vector<uint8_t> bye(1, 0xBB);
    wr(*conn, bye).wait();
    double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    std::printf("burst of %zu x %zu B: %s  %.1f ms (%.0f msg/s)\n", kBurst, kBurstSize, done[0] == 0xD0 ? "ack ok" : "BAD ACK", ms, kBurst / ms * 1000);
    if (done[0] != 0xD0) bad++;
  }
  std::printf("%s: %s\n", server ? "server" : "client", bad ? "FAILED" : "PASSED");
  conn->close();
  ctx->close();
  ctx->join();
  return bad ? 1 : 0;
}
