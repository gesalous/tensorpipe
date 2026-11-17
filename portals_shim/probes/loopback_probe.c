/*
 * Probe: what does BXIv2 do with memory that was never registered?
 *
 * One process, one non-matching physical NI, a PUT to itself (loopback) through
 * the exact resources the shim uses: a whole-address-space MD and a persistent
 * whole-address-space LE (ptl_device.c). Each case moves a pattern between
 * buffers prepared in a particular way and checks the bytes and the events. The
 * NIC's V2P counters (sysfs counters/v2p/, see CD below) are read around each
 * case to show whether the NIC walked page tables.
 *
 *   build: cc -O2 -o loopback_probe loopback_probe.c -lportals
 *   run:   ./loopback_probe [fault] [fork] [unmapped]   (extra cases: COW after fork; an unmapped target)
 */
#include <portals4.h>
#include <portals4_bxiext.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CD "/sys/class/bxi/bxi0/counters/v2p/"
#define CK(c) do { int r_ = (c); if (r_ != PTL_OK) { fprintf(stderr, "FATAL %s rc=%d (%s)\n", #c, r_, PtlToStr(r_, PTL_STR_ERROR)); _exit(1); } } while (0)

static ptl_handle_ni_t ni;
static ptl_handle_eq_t eq;
static ptl_handle_md_t md;
static ptl_pt_index_t pte;
static ptl_process_t self;

static double rdc(const char *f)
{
	char p[128]; FILE *fp; double v = -1;
	snprintf(p, sizeof p, CD "%s", f);
	if ((fp = fopen(p, "r")) != NULL) { if (fscanf(fp, "%lf", &v) != 1) v = -1; fclose(fp); }
	return v;
}
struct cnt { double ph, pm, dh, dm, rd; };
static void snap(struct cnt *c)
{
	c->ph = rdc("pte_hit"); c->pm = rdc("pte_miss"); c->dh = rdc("pde_hit"); c->dm = rdc("pde_miss"); c->rd = rdc("read");
}
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

/* Waits for the ACK and the PUT of one operation. Returns 0 on success. */
static int wait_put(size_t len, uint64_t hdr, int verbose)
{
	int got_ack = 0, got_put = 0, got_send = 0, rc = 0;
	double t0 = now();
	ptl_event_t ev;

	while (!(got_ack && got_put) && now() - t0 < 3.0) {
		int r = PtlEQGet(eq, &ev);
		if (r == PTL_EQ_EMPTY) continue;
		if (r != PTL_OK && r != PTL_EQ_DROPPED) { printf("   EQGet rc=%d\n", r); return -1; }
		if (verbose)
			printf("   event %-12s fail=%d(%s) rlength=%lu mlength=%lu hdr=0x%lx\n", PtlToStr(ev.type, PTL_STR_EVENT),
			       ev.ni_fail_type, PtlToStr(ev.ni_fail_type, PTL_STR_FAIL_TYPE), ev.rlength, ev.mlength, ev.hdr_data);
		if (ev.ni_fail_type != PTL_NI_OK) rc = -2;
		if (ev.type == PTL_EVENT_ACK) got_ack = 1;
		if (ev.type == PTL_EVENT_PUT) {
			got_put = 1;
			if (ev.hdr_data != hdr || ev.rlength != len) { printf("   PUT mismatch hdr/len\n"); rc = -3; }
		}
		if (ev.type == PTL_EVENT_SEND) got_send = 1;
	}
	(void)got_send;
	if (!(got_ack && got_put)) { printf("   TIMEOUT ack=%d put=%d\n", got_ack, got_put); return -4; }
	return rc;
}

static int do_put(const void *src, void *dst, size_t len, uint64_t hdr, int verbose)
{
	CK(PtlPut(md, (ptl_size_t)(uintptr_t)src, len, PTL_ACK_REQ, self, pte, 0, (ptl_size_t)(uintptr_t)dst, NULL, hdr));
	return wait_put(len, hdr, verbose);
}

static void report(const char *name, int rc, int data_ok, const struct cnt *a, const struct cnt *b, double us)
{
	printf("%-46s %s data=%s | v2p d: pte_hit %+.0f pte_miss %+.0f pde_hit %+.0f pde_miss %+.0f read %+.0f | %.0f us\n",
	       name, rc == 0 ? "OK  " : "FAIL", data_ok ? "ok " : "BAD", b->ph - a->ph, b->pm - a->pm, b->dh - a->dh, b->dm - a->dm,
	       b->rd - a->rd, us);
	fflush(stdout);
}

static void *map(size_t len, int flags) { void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | flags, -1, 0); if (p == MAP_FAILED) { perror("mmap"); _exit(1); } return p; }

/* One case: src filled with `pat`, dst prepared by the caller. */
static void put_case(const char *name, const void *src, void *dst, size_t len, unsigned char pat, int verbose)
{
	struct cnt a, b; double t0; int rc, ok;
	snap(&a); t0 = now();
	rc = do_put(src, dst, len, 0xABCD000000000000ULL | len, verbose);
	double us = (now() - t0) * 1e6;
	snap(&b);
	ok = 1;
	for (size_t i = 0; i < len; i += 4096 < len ? 4096 : len + 1) if (((unsigned char *)dst)[i] != pat) { ok = 0; break; }
	if (len && ((unsigned char *)dst)[len - 1] != pat) ok = 0;
	report(name, rc, ok, &a, &b, us);
}

int main(int argc, char **argv)
{
	int unmapped = 0, dofork = 0, dofault = 0;
	for (int i = 1; i < argc; i++) { unmapped |= !strcmp(argv[i], "unmapped"); dofork |= !strcmp(argv[i], "fork"); dofault |= !strcmp(argv[i], "fault"); }
	ptl_ni_limits_t lim; ptl_md_t mdd; ptl_le_t le; ptl_handle_le_t leh; size_t MB2 = 2UL << 20;

	setvbuf(stdout, NULL, _IONBF, 0);
	CK(PtlInit());
	CK(PtlNIInit(PTL_IFACE_DEFAULT, PTL_NI_NO_MATCHING | PTL_NI_PHYSICAL, PTL_PID_ANY, NULL, &lim, &ni));
	CK(PtlGetPhysId(ni, &self));
	CK(PtlEQAlloc(ni, 4096, &eq));
	memset(&mdd, 0, sizeof mdd); mdd.start = 0; mdd.length = PTL_SIZE_MAX; mdd.eq_handle = eq; mdd.ct_handle = PTL_CT_NONE;
	CK(PtlMDBind(ni, &mdd, &md));
	CK(PtlPTAlloc(ni, 0, eq, PTL_PT_ANY, &pte));
	CK(PtlPTEnable(ni, pte));
	memset(&le, 0, sizeof le); le.start = 0; le.length = PTL_SIZE_MAX; le.ct_handle = PTL_CT_NONE; le.uid = PTL_UID_ANY;
	le.match_id.phys.nid = PTL_NID_ANY; le.match_id.phys.pid = PTL_PID_ANY;
	le.options = PTL_LE_OP_PUT | PTL_LE_OP_GET | PTL_LE_IS_ACCESSIBLE | PTL_LE_EVENT_LINK_DISABLE;
	CK(PtlLEAppend(ni, pte, &le, PTL_PRIORITY_LIST, NULL, &leh));
	printf("self nid %u pid %u pte %u; v2p counters %s\n", self.phys.nid, self.phys.pid, pte, rdc("pte_hit") >= 0 ? "readable" : "NOT readable");

	/* 0. zero length, with header data (an ack/send-with-imm looks like this) */
	{ struct cnt a, b; snap(&a); int rc = do_put(NULL, NULL, 0, 0x1122334455667788ULL, 1); snap(&b); report("0. zero-length put, no buffers at all", rc, 1, &a, &b, 0); }

	unsigned char *src = map(MB2, 0), *dst = map(MB2, 0);
	memset(src, 0xA5, MB2);

	/* 1. small, both touched */
	memset(dst, 0, MB2);
	put_case("1. 384 B, src+dst touched", src, dst, 384, 0xA5, 1);
	/* 2. 4 KiB, touched */
	put_case("2. 4 KiB, src+dst touched", src, dst, 4096, 0xA5, 0);
	/* 3. 2 MiB, touched (a tensorpipe ring) */
	put_case("3. 2 MiB, src+dst touched", src, dst, MB2, 0xA5, 0);

	/* 4. destination never touched, fresh anonymous mmap */
	{ unsigned char *d = map(MB2, 0); put_case("4. 4 KiB into UNTOUCHED fresh mmap", src, d, 4096, 0xA5, 1); munmap(d, MB2); }
	{ unsigned char *d = map(MB2, 0); put_case("5. 2 MiB into UNTOUCHED fresh mmap (a ring)", src, d, MB2, 0xA5, 0); munmap(d, MB2); }

	/* 6. destination pages dropped by the kernel after being used */
	memset(dst, 0x11, MB2);
	if (madvise(dst, MB2, MADV_DONTNEED)) perror("madvise");
	put_case("6. 2 MiB into pages dropped by MADV_DONTNEED", src, dst, MB2, 0xA5, 0);

	/* 7. sparse 1 GiB, write in the middle */
	{ size_t G = 1UL << 30; unsigned char *d = map(G, MAP_NORESERVE); put_case("7. 4 KiB into middle of untouched 1 GiB", src, d + G / 2, 4096, 0xA5, 0); munmap(d, G); }

	/* 8. source never touched (zero pages), destination touched with another pattern */
	{ unsigned char *s = map(MB2, 0); memset(dst, 0x77, MB2); put_case("8. 2 MiB from UNTOUCHED source (zero pages)", s, dst, MB2, 0x00, 0); munmap(s, MB2); }

	/* 9. source dropped after use */
	memset(src, 0x3C, MB2); madvise(src, MB2, MADV_DONTNEED);
	memset(dst, 0x77, MB2);
	put_case("9. 2 MiB from source dropped by MADV_DONTNEED", src, dst, MB2, 0x00, 0);

	/* 10. a burst of small puts back to back, delivery order seen by the target */
	{
		enum { N = 2000 }; uint64_t seq_seen[N]; int n = 0; ptl_event_t ev; double t0 = now(); int sent = 0, acks = 0;
		memset(src, 0x5A, 4096);
		struct cnt a, b; snap(&a);
		for (int i = 0; i < N; i++) CK(PtlPut(md, (ptl_size_t)(uintptr_t)src, 64, PTL_ACK_REQ, self, pte, 0, (ptl_size_t)(uintptr_t)dst + 64 * (i % 1000), NULL, (uint64_t)i));
		sent = N;
		while ((n < N || acks < N) && now() - t0 < 5.0) {
			int r = PtlEQGet(eq, &ev); if (r == PTL_EQ_EMPTY) continue;
			if (ev.type == PTL_EVENT_PUT && n < N) seq_seen[n++] = ev.hdr_data;
			if (ev.type == PTL_EVENT_ACK) acks++;
		}
		snap(&b);
		int inversions = 0; for (int i = 1; i < n; i++) if (seq_seen[i] < seq_seen[i - 1]) inversions++;
		printf("10. %d back-to-back 64 B puts (loopback): puts seen %d acks %d, order inversions at the target: %d\n", sent, n, acks, inversions);
		(void)b; (void)a;
	}


	if (dofault) {
		/* 13. Ways to take the page faults out of the NIC's way, before the first PUT.
		 * Each case uses a fresh 2 MiB mapping and times only the PUT; the prefault cost is printed apart. */
		memset(src, 0xA5, MB2); /* case 9 left src zeroed */
		const char *names[] = {"13a. baseline: untouched", "13b. MAP_POPULATE", "13c. atomic-or-0 touch each page", "13d. mlock()"};
		for (int k = 0; k < 4; k++) {
			unsigned char *d = map(MB2, k == 1 ? MAP_POPULATE : 0);
			double t0 = now(); int rc0 = 0;
			if (k == 2) for (size_t o = 0; o < MB2; o += 4096) __atomic_fetch_or(&d[o], 0, __ATOMIC_RELAXED);
			if (k == 3) rc0 = mlock(d, MB2);
			double pre = (now() - t0) * 1e6;
			char label[96]; snprintf(label, sizeof label, "%s (prefault %.0f us%s)", names[k], pre, rc0 ? ", mlock FAILED" : "");
			put_case(label, src, d, MB2, 0xA5, 0);
			munmap(d, MB2);
		}
	}

	if (dofork) {
		/* 12. The NIC has translated this buffer. fork() makes it copy-on-write. Does a
		 * later PUT into the parent's buffer leak into the child's copy? Correct: the
		 * child still sees the pre-fork byte, the parent sees the new one. */
		unsigned char *f = map(MB2, 0);
		int to_child[2], from_child[2]; pid_t pid; unsigned char child_sees = 0, go = 1;
		memset(src, 0xA5, 4096); memset(f, 0xE1, MB2);
		put_case("12a. prime: put 0xA5 into f (NIC translates f)", src, f, 4096, 0xA5, 0);
		if (pipe(to_child) || pipe(from_child)) { perror("pipe"); _exit(1); }
		pid = fork();
		if (pid == 0) { unsigned char g; if (read(to_child[0], &g, 1) != 1) _exit(2); g = f[0]; if (write(from_child[1], &g, 1) != 1) _exit(3); _exit(0); }
		memset(src, 0xC3, 4096);
		put_case("12b. after fork: put 0xC3 into parent's f", src, f, 4096, 0xC3, 0);
		if (write(to_child[1], &go, 1) != 1) perror("write");
		if (read(from_child[0], &child_sees, 1) != 1) perror("read");
		waitpid(pid, NULL, 0);
		printf("12c. child reads f[0] = 0x%02x -> %s\n", child_sees, child_sees == 0xA5 ? "CORRECT (copy-on-write respected)" : child_sees == 0xC3 ? "LEAK: child sees the parent's PUT" : "UNEXPECTED");
		munmap(f, MB2);
	}

	if (unmapped) {
		unsigned char *d = map(MB2, 0); munmap(d, MB2);
		printf("11. destination UNMAPPED (expect an error, not a hang)\n");
		memset(src, 0xA5, 4096);
		{ /* never read d: it is unmapped. Only the events and the outcome matter. */
			struct cnt a, b; double t0 = now(); snap(&a);
			int rc = do_put(src, d, 4096, 0xABCD000000001000ULL, 1);
			snap(&b);
			printf("11. put to unmapped range returned %d after %.0f us; v2p d: pte_miss %+.0f read %+.0f\n", rc, (now() - t0) * 1e6, b.pm - a.pm, b.rd - a.rd);
		}
		/* does the NI still work afterwards? */
		{ unsigned char *ok = map(MB2, 0); put_case("11b. normal put right after the failure", src, ok, 4096, 0xA5, 1); }
	}
	return 0;
}
