/*
 * Internals of the tensorpipe-over-Portals4 verbs shim. See design.md.
 */
#ifndef PTL_INTERNAL_H
#define PTL_INTERNAL_H

#include <infiniband/verbs.h> /* types only: we never call ibv_* */
#include <portals4.h>
#include <portals4_bxiext.h> /* PtlToStr */
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PTL_EXPORT __attribute__((visibility("default")))
#define PTL_CONTAINER_OF(ptr, type, member) \
	((type *)((char *)(ptr) - offsetof(type, member)))

/* ---- logging: TP_PTL_LOG=0 fatal only, 1 warnings (default), 2 debug ----- */
void ptl_log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void ptl_fatal(const char *file, int line, const char *fmt, ...)
	__attribute__((noreturn, format(printf, 3, 4)));

#define PTL_FATAL(...) ptl_fatal(__FILE__, __LINE__, __VA_ARGS__)
#define PTL_WARN(...) ptl_log(1, __VA_ARGS__)
#define PTL_DEBUG(...) ptl_log(2, __VA_ARGS__)
#define PTL_UNIMPLEMENTED() PTL_FATAL("%s is unimplemented", __func__)

/* ---- sizing ------------------------------------------------------------- */
#define PTL_EQ_SIZE 65536 /* events in each context's event queue */

/* ---- process-wide network interface (ptl_ni.c) -------------------------- */
struct ptl_ni {
	ptl_handle_ni_t handle;
	ptl_process_t id; /* physical nid/pid of this process */
};

/* Lazily initialised, released at exit. NULL (errno set) if Portals is unusable. */
const struct ptl_ni *ptl_ni_get(void);

/* ---- peer address, carried in the 16 byte gid (ptl_device.c) ------------- */
struct ptl_gid {
	char magic[4]; /* "PTL4": lets us reject foreign gids */
	uint32_t nid;
	uint32_t pid;
	uint32_t pte;
} __attribute__((packed));
_Static_assert(sizeof(struct ptl_gid) == sizeof(union ibv_gid),
	       "ptl_gid must fill an ibv_gid");

/* ---- objects. The ibv_* struct is embedded so the pointer handed to
 * tensorpipe is a real ibv_* pointer; magic guards against foreign pointers. */
#define PTL_MAGIC_CTX 0x5054434eU /* "PTCN" */
#define PTL_MAGIC_PD 0x50545044U
#define PTL_MAGIC_CQ 0x50544351U
#define PTL_MAGIC_SRQ 0x50545352U

struct ptl_ctx {
	uint32_t magic;
	struct ibv_context ibv;
	const struct ptl_ni *ni;
	ptl_handle_eq_t eq; /* every event of this context */
	ptl_handle_md_t md; /* whole address space, for sends (later sprints) */
	ptl_pt_index_t pte; /* the one PTE peers target */
	ptl_handle_le_t le; /* persistent whole-address-space LE on that PTE */
	pthread_mutex_t lock; /* serialises draining the EQ */
	int open_objects;     /* pds + cqs + srqs still alive: leak check */
};

struct ptl_pd {
	uint32_t magic;
	struct ibv_pd ibv;
	struct ptl_ctx *ctx;
};

struct ptl_cq {
	uint32_t magic;
	struct ibv_cq ibv;
	struct ptl_ctx *ctx;
};

/* The SRQ is accounting only: Portals needs no posted receive buffer for a PUT
 * into the persistent LE (design.md 5.4). We keep the posted wr_ids because a
 * receive completion reports the wr_id of the receive it consumed. */
struct ptl_srq {
	uint32_t magic;
	struct ibv_srq ibv;
	struct ptl_pd *pd;
	pthread_mutex_t lock;
	uint64_t *wr_ids; /* ring of posted receive wr_ids */
	uint32_t cap, head, count;
};

static inline struct ptl_ctx *ptl_ctx_from_ibv(struct ibv_context *c)
{
	struct ptl_ctx *ctx = PTL_CONTAINER_OF(c, struct ptl_ctx, ibv);
	if (ctx->magic != PTL_MAGIC_CTX)
		PTL_FATAL("not a ptl context: %p", (void *)c);
	return ctx;
}

static inline struct ptl_pd *ptl_pd_from_ibv(struct ibv_pd *p)
{
	struct ptl_pd *pd = PTL_CONTAINER_OF(p, struct ptl_pd, ibv);
	if (pd->magic != PTL_MAGIC_PD)
		PTL_FATAL("not a ptl pd: %p", (void *)p);
	return pd;
}

static inline struct ptl_cq *ptl_cq_from_ibv(struct ibv_cq *c)
{
	struct ptl_cq *cq = PTL_CONTAINER_OF(c, struct ptl_cq, ibv);
	if (cq->magic != PTL_MAGIC_CQ)
		PTL_FATAL("not a ptl cq: %p", (void *)c);
	return cq;
}

static inline struct ptl_srq *ptl_srq_from_ibv(struct ibv_srq *s)
{
	struct ptl_srq *srq = PTL_CONTAINER_OF(s, struct ptl_srq, ibv);
	if (srq->magic != PTL_MAGIC_SRQ)
		PTL_FATAL("not a ptl srq: %p", (void *)s);
	return srq;
}

#endif /* PTL_INTERNAL_H */
