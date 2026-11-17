/*
 * Direct test of ptl_reg_mr / ptl_dereg_mr pinning (no tensorpipe). Loads the shim
 * like tensorpipe does (dlopen, ptl_ symbols) and watches VmLck in /proc/self/status.
 *
 *   build: cc -O2 -o mr_pin_test mr_pin_test.c -ldl
 *   run:   ./mr_pin_test /path/to/libtensorpipe_portals.so
 */
#include <dlfcn.h>
#include <errno.h>
#include <infiniband/verbs.h> /* types only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static long vmlck_kb(void)
{
	char l[256]; long v = -1; FILE *f = fopen("/proc/self/status", "r");
	while (f && fgets(l, sizeof l, f)) if (sscanf(l, "VmLck: %ld kB", &v) == 1) break;
	if (f) fclose(f);
	return v;
}
static int fails;
static void expect(const char *what, long got, long want)
{
	printf("%-62s VmLck=%5ld kB  %s\n", what, got, got == want ? "ok" : "WRONG");
	if (got != want) fails++;
}

int main(int argc, char **argv)
{
	void *h = dlopen(argc > 1 ? argv[1] : "./libtensorpipe_portals.so", RTLD_NOW | RTLD_LOCAL);
	if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
	typedef struct ibv_device **(*f_gdl)(int *);
	typedef struct ibv_context *(*f_open)(struct ibv_device *);
	typedef struct ibv_pd *(*f_apd)(struct ibv_context *);
	typedef struct ibv_mr *(*f_reg)(struct ibv_pd *, void *, size_t, int);
	typedef int (*f_dereg)(struct ibv_mr *);
	typedef int (*f_dpd)(struct ibv_pd *);
	typedef int (*f_close)(struct ibv_context *);
	f_gdl get_device_list = (f_gdl)dlsym(h, "ptl_get_device_list");
	f_open open_device = (f_open)dlsym(h, "ptl_open_device");
	f_apd alloc_pd = (f_apd)dlsym(h, "ptl_alloc_pd");
	f_reg reg_mr = (f_reg)dlsym(h, "ptl_reg_mr");
	f_dereg dereg_mr = (f_dereg)dlsym(h, "ptl_dereg_mr");
	f_dpd dealloc_pd = (f_dpd)dlsym(h, "ptl_dealloc_pd");
	f_close close_device = (f_close)dlsym(h, "ptl_close_device");
	if (!get_device_list || !open_device || !alloc_pd || !reg_mr || !dereg_mr || !dealloc_pd || !close_device) { fprintf(stderr, "missing symbol\n"); return 1; }

	int n; struct ibv_device **l = get_device_list(&n);
	if (n < 1) { fprintf(stderr, "no device\n"); return 1; }
	struct ibv_context *ctx = open_device(l[0]);
	struct ibv_pd *pd = alloc_pd(ctx);
	const size_t P = 4096; const int ACC = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
	char *buf = mmap(NULL, 64 * P, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	long base = vmlck_kb();
	expect("start", base, base);

	struct ibv_mr *a = reg_mr(pd, buf, 8 * P, ACC);               /* pages 0..7  */
	expect("A = pages 0-7 registered (+32 kB)", vmlck_kb(), base + 32);
	struct ibv_mr *b = reg_mr(pd, buf + 4 * P, 8 * P, ACC);       /* pages 4..11, overlaps A on 4..7 */
	expect("B = pages 4-11, overlaps A (+16 kB: 8-11 only)", vmlck_kb(), base + 48);
	dereg_mr(a);
	expect("dereg A: pages 4-7 stay locked for B (A-only 0-3 freed)", vmlck_kb(), base + 32);
	struct ibv_mr *c = reg_mr(pd, buf + 5 * P + 100, 100, ACC);   /* unaligned, inside B, 1 page */
	expect("C = 100 B inside page 5 (already locked by B)", vmlck_kb(), base + 32);
	dereg_mr(b);
	expect("dereg B: only page 5 stays (C)", vmlck_kb(), base + 4);
	dereg_mr(c);
	expect("dereg C: everything unlocked", vmlck_kb(), base);

	/* errors */
	errno = 0; struct ibv_mr *z = reg_mr(pd, buf, 0, ACC);
	printf("%-62s %s (errno %d)\n", "zero length", z ? "WRONG" : "rejected", errno); if (z) fails++;
	errno = 0; munmap(buf + 20 * P, 4 * P); z = reg_mr(pd, buf + 18 * P, 8 * P, ACC);
	printf("%-62s %s (errno %d = %s)\n", "range with an unmapped hole", z ? "WRONG" : "rejected", errno, errno == EFAULT ? "EFAULT ok" : "unexpected");
	if (z || errno != EFAULT) fails++;
	expect("after the failures nothing is left locked", vmlck_kb(), base);

	dealloc_pd(pd); close_device(ctx);
	printf("%s\n", fails ? "FAILED" : "ALL OK");
	return fails != 0;
}
