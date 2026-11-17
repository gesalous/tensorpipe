/*
 * Memory regions (sprint 2 / test tier T2).
 *
 * BXIv2 resolves buffer addresses itself, so a region creates no Portals object
 * (design.md 9.3). What reg_mr does:
 *   - checks the range is mapped (EFAULT otherwise),
 *   - pins it with mlock until dereg_mr, so the NIC never takes a page fault
 *     (the NIC's fault path is about 5x slower than the CPU's),
 *   - remembers it, so post_send can reject a bad local sge instead of letting
 *     the driver SIGSEGV this process.
 * lkey == rkey == a non-zero handle; rkeys are not enforced (design.md 5.3).
 *
 * TP_PTL_PIN=0 turns pinning off (the range is still prefaulted).
 */
#include "ptl_internal.h"
#include <errno.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

static struct {
	pthread_mutex_t lock;
	struct ptl_mr *head;
	uint32_t next_key;
} g_mr = {.lock = PTHREAD_MUTEX_INITIALIZER, .next_key = 1};

static bool ptl_pin_enabled(void)
{
	const char *e = getenv("TP_PTL_PIN");
	return !(e != NULL && e[0] == '0');
}

static bool ptl_range_mapped(uintptr_t start, uintptr_t end)
{
	long pg = sysconf(_SC_PAGESIZE);
	size_t pages = (end - start) / (size_t)pg;
	unsigned char *vec = malloc(pages);
	bool ok;

	if (vec == NULL)
		return true; /* cannot tell: do not blame the address */
	ok = mincore((void *)start, end - start, vec) == 0;
	free(vec);
	return ok;
}

struct seg {
	uintptr_t s, e;
};

/* munlock the part of mr's range that no other live pinned MR still covers
 * (munlock is not reference counted). Called with g_mr.lock held, mr unlinked. */
static void ptl_unpin_unshared(const struct ptl_mr *mr)
{
	size_t n = 0, cap, i;
	struct seg *segs;
	const struct ptl_mr *o;

	for (o = g_mr.head; o != NULL; o = o->next)
		n++;
	cap = n + 2;
	segs = malloc(cap * sizeof(*segs));
	if (segs == NULL) {
		PTL_WARN("out of memory unlocking %p: leaving the range locked", (void *)mr->pstart);
		return;
	}
	segs[0].s = mr->pstart;
	segs[0].e = mr->pend;
	n = 1;
	for (o = g_mr.head; o != NULL; o = o->next) {
		if (!o->pinned)
			continue;
		for (i = 0; i < n; i++) {
			if (o->pend <= segs[i].s || o->pstart >= segs[i].e)
				continue; /* no overlap */
			if (o->pstart > segs[i].s && o->pend < segs[i].e) { /* split in two */
				segs[n].s = o->pend;
				segs[n].e = segs[i].e;
				segs[i].e = o->pstart;
				n++;
			} else if (o->pstart <= segs[i].s && o->pend >= segs[i].e) {
				segs[i].e = segs[i].s; /* fully covered: empty */
			} else if (o->pstart <= segs[i].s) {
				segs[i].s = o->pend;
			} else {
				segs[i].e = o->pstart;
			}
		}
	}
	for (i = 0; i < n; i++)
		if (segs[i].e > segs[i].s && munlock((void *)segs[i].s, segs[i].e - segs[i].s) != 0)
			PTL_WARN("munlock failed: %d", errno);
	free(segs);
}

/* mlock is not atomic: over a range with a hole it locks the mapped prefix and then
 * fails. Undo that, without touching pages another live MR holds. */
static void ptl_undo_partial_lock(const struct ptl_mr *mr)
{
	pthread_mutex_lock(&g_mr.lock);
	ptl_unpin_unshared(mr);
	pthread_mutex_unlock(&g_mr.lock);
}

PTL_EXPORT struct ibv_mr *ptl_reg_mr(struct ibv_pd *ibv_pd, void *addr, size_t length, int access)
{
	struct ptl_pd *pd = ptl_pd_from_ibv(ibv_pd);
	long pg = sysconf(_SC_PAGESIZE);
	uintptr_t a = (uintptr_t)addr;
	struct ptl_mr *mr;
	bool want_pin = ptl_pin_enabled(), locked = false;

	if (addr == NULL || length == 0 || a + length < a) {
		errno = EINVAL;
		return NULL;
	}
	mr = calloc(1, sizeof(*mr));
	if (mr == NULL) {
		errno = ENOMEM;
		return NULL;
	}
	mr->pstart = a & ~((uintptr_t)pg - 1);
	mr->pend = (a + length + (uintptr_t)pg - 1) & ~((uintptr_t)pg - 1);

	if (mlock((void *)mr->pstart, mr->pend - mr->pstart) == 0) {
		locked = true;
	} else {
		int err = errno;
		ptl_undo_partial_lock(mr);
		if (!ptl_range_mapped(mr->pstart, mr->pend)) {
			free(mr);
			errno = EFAULT;
			return NULL;
		}
		if (want_pin) {
			PTL_WARN("mlock of %zu bytes failed (%d): raise RLIMIT_MEMLOCK ('ulimit -l') or set TP_PTL_PIN=0",
				 length, err);
			free(mr);
			errno = ENOMEM;
			return NULL;
		}
	}
	if (!want_pin && locked)
		munlock((void *)mr->pstart, mr->pend - mr->pstart); /* prefault only */
	mr->pinned = want_pin && locked;

	mr->magic = PTL_MAGIC_MR;
	mr->pd = pd;
	mr->ibv.context = ibv_pd->context;
	mr->ibv.pd = ibv_pd;
	mr->ibv.addr = addr;
	mr->ibv.length = length;
	(void)access; /* rkeys are not enforced */

	pthread_mutex_lock(&g_mr.lock);
	mr->ibv.handle = g_mr.next_key++;
	if (g_mr.next_key == 0)
		g_mr.next_key = 1;
	mr->ibv.lkey = mr->ibv.rkey = mr->ibv.handle;
	mr->next = g_mr.head;
	g_mr.head = mr;
	pthread_mutex_unlock(&g_mr.lock);
	return &mr->ibv;
}

PTL_EXPORT int ptl_dereg_mr(struct ibv_mr *ibv_mr)
{
	struct ptl_mr *mr = ptl_mr_from_ibv(ibv_mr);
	struct ptl_mr **pp;

	pthread_mutex_lock(&g_mr.lock);
	for (pp = &g_mr.head; *pp != NULL && *pp != mr; pp = &(*pp)->next)
		;
	if (*pp == NULL) {
		pthread_mutex_unlock(&g_mr.lock);
		PTL_FATAL("dereg_mr of an MR that is not registered: %p", (void *)mr);
	}
	*pp = mr->next;
	if (mr->pinned)
		ptl_unpin_unshared(mr);
	pthread_mutex_unlock(&g_mr.lock);

	mr->magic = 0;
	free(mr);
	return 0;
}
