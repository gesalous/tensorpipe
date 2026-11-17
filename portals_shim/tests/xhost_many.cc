// Many concurrent connections between two processes over the ibv transport on the
// Portals shim. Every connection echoes messages of assorted sizes and then a burst
// of small ones, all connections at the same time (one thread each), so they share
// the context's single EQ, CQ, SRQ and LE.
//   server: xhost_many server <addr-file> <ip> <connections>
//   client: xhost_many client <addr-file> <connections>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <tensorpipe/transport/connection.h>
#include <tensorpipe/transport/ibv/factory.h>
#include <tensorpipe/transport/listener.h>

using namespace tensorpipe;
using Clock = std::chrono::steady_clock;

static std::atomic<int> gBad{0};
static std::atomic<uint64_t> gBytes{0};

static uint8_t pat(uint64_t conn, uint64_t msg, size_t k) { return (uint8_t)(conn * 29 + msg * 131 + k * 7 + (k >> 8)); }
static void fill(std::vector<uint8_t>& b, uint64_t c, uint64_t m) { for (size_t k = 0; k < b.size(); k++) b[k] = pat(c, m, k); }
static bool check(const std::vector<uint8_t>& b, uint64_t c, uint64_t m) {
  for (size_t k = 0; k < b.size(); k++) if (b[k] != pat(c, m, k)) { std::fprintf(stderr, "conn %lu msg %lu: byte %zu wrong\n", c, m, k); return false; }
  return true;
}
static void die(const Error& e, const char* what) { std::fprintf(stderr, "%s: %s\n", what, e.what().c_str()); std::_Exit(2); }

static void rd(transport::Connection& c, std::vector<uint8_t>& b) {
  std::promise<void> p;
  c.read(b.data(), b.size(), [&](const Error& e, const void*, size_t) { if (e) die(e, "read"); p.set_value(); });
  p.get_future().wait();
}
static std::future<void> wr(transport::Connection& c, const std::vector<uint8_t>& b) {
  auto p = std::make_shared<std::promise<void>>();
  c.write(b.data(), b.size(), [p](const Error& e) { if (e) die(e, "write"); p->set_value(); });
  return p->get_future();
}

static const size_t kSizes[] = {1, 100, 4096, 65536, 300000, 3 * 1024 * 1024 + 17};
static const size_t kRounds = 12, kBurst = 200, kBurstSize = 64;

static size_t sizeOf(uint64_t conn, size_t round) { return kSizes[(conn + round) % (sizeof(kSizes) / sizeof(kSizes[0]))]; }

static void serveOne(std::shared_ptr<transport::Connection> c) {
  std::vector<uint8_t> hello(8);
  rd(*c, hello);
  uint64_t id; std::memcpy(&id, hello.data(), 8);
  for (size_t r = 0; r < kRounds; r++) {
    std::vector<uint8_t> b(sizeOf(id, r));
    rd(*c, b);
    if (!check(b, id, r)) gBad++;
    wr(*c, b).wait();
    gBytes += 2 * b.size();
  }
  for (size_t i = 0; i < kBurst; i++) {
    std::vector<uint8_t> b(kBurstSize);
    rd(*c, b);
    if (!check(b, id, 1000 + i)) gBad++;
  }
  std::vector<uint8_t> done(1, 0xD0);
  wr(*c, done).wait();
  std::vector<uint8_t> bye(1);
  rd(*c, bye);
  std::this_thread::sleep_for(std::chrono::milliseconds(500)); // let the peer finish reading before EOF
  c->close();
}

static void driveOne(std::shared_ptr<transport::Connection> c, uint64_t id) {
  std::vector<uint8_t> hello(8);
  std::memcpy(hello.data(), &id, 8);
  wr(*c, hello).wait();
  for (size_t r = 0; r < kRounds; r++) {
    std::vector<uint8_t> b(sizeOf(id, r));
    fill(b, id, r);
    auto w = wr(*c, b);
    std::vector<uint8_t> back(b.size());
    rd(*c, back);
    w.wait();
    if (!check(back, id, r)) gBad++;
    gBytes += 2 * b.size();
  }
  std::vector<std::vector<uint8_t>> msgs(kBurst, std::vector<uint8_t>(kBurstSize));
  std::vector<std::future<void>> fs;
  for (size_t i = 0; i < kBurst; i++) { fill(msgs[i], id, 1000 + i); fs.push_back(wr(*c, msgs[i])); }
  std::vector<uint8_t> done(1);
  rd(*c, done);
  for (auto& f : fs) f.wait();
  if (done[0] != 0xD0) gBad++;
  std::vector<uint8_t> bye(1, 0xBB);
  wr(*c, bye).wait();
  std::this_thread::sleep_for(std::chrono::milliseconds(500)); // the peer may still be polling our last message
  c->close();
}

int main(int argc, char** argv) {
  if (argc < 4) { std::fprintf(stderr, "usage: server <file> <ip> <n> | client <file> <n>\n"); return 1; }
  bool server = !std::strcmp(argv[1], "server");
  int n = std::atoi(argv[server ? 4 : 3]);
  auto ctx = transport::ibv::create();
  if (!ctx->isViable()) { std::fprintf(stderr, "ibv transport is not viable (TP_IBV_LIBRARY set?)\n"); return 1; }
  std::vector<std::thread> threads;
  std::string addr; // outlives the connection threads
  auto t0 = Clock::now();
  if (server) {
    auto l = ctx->listen(argv[3]);
    { std::ofstream f(std::string(argv[2]) + ".tmp"); f << l->addr(); }
    std::rename((std::string(argv[2]) + ".tmp").c_str(), argv[2]);
    std::printf("listening on %s for %d connections\n", l->addr().c_str(), n);
    std::mutex m;
    std::vector<std::shared_ptr<transport::Connection>> accepted;
    std::promise<void> all;
    std::function<void()> next = [&]() {
      l->accept([&](const Error& e, std::shared_ptr<transport::Connection> c) {
        if (e) die(e, "accept");
        std::lock_guard<std::mutex> g(m);
        accepted.push_back(c);
        if ((int)accepted.size() == n) all.set_value(); else next();
      });
    };
    next();
    all.get_future().wait();
    std::printf("all %d accepted after %.0f ms\n", n, std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
    for (auto& c : accepted) threads.emplace_back(serveOne, c);
  } else {
    for (int i = 0; i < 600 && addr.empty(); i++) { std::ifstream f(argv[2]); std::getline(f, addr); if (addr.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
    if (addr.empty()) { std::fprintf(stderr, "no address file\n"); return 1; }
    for (int i = 0; i < n; i++) threads.emplace_back([&, i]() { driveOne(ctx->connect(addr), i); });
  }
  for (auto& t : threads) t.join();
  double s = std::chrono::duration<double>(Clock::now() - t0).count();
  std::printf("%s: %d connections, %.0f MB echoed in %.2f s (%.0f MB/s): %s\n", server ? "server" : "client", n,
              gBytes.load() / 1e6, s, gBytes.load() / 1e6 / s, gBad ? "FAILED" : "PASSED");
  ctx->close();
  ctx->join();
  return gBad ? 1 : 0;
}
