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
#define PTL_MAGIC_MR 0x50544d52U
#define PTL_MAGIC_QP 0x50545150U

/* qpn travels in 16 bits of hdr_data (design.md 9.2); 0 is never assigned. */
#define PTL_MAX_QPN 65535U

struct ptl_ctx {
	uint32_t magic;
	struct ibv_context ibv;
	const struct ptl_ni *ni;
	ptl_handle_eq_t eq; /* every event of this context */
	ptl_handle_md_t md; /* whole address space, for sends (later sprints) */
	ptl_pt_index_t pte; /* the one PTE peers target */
	ptl_handle_le_t le; /* persistent whole-address-space LE on that PTE */
	pthread_mutex_t lock; /* serialises draining the EQ */
	int open_objects;     /* pds, cqs, srqs, qps still alive: leak check */
	struct ptl_qp **qps;  /* qpn -> qp, PTL_MAX_QPN + 1 slots */
	pthread_mutex_t qp_lock;
	uint32_t next_qpn;
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

/* Address of the peer end of a connection, from the gid given to modify_qp(RTR). */
struct ptl_peer {
	uint32_t nid, pid, pte;
	uint32_t qpn;
};

struct ptl_qp {
	uint32_t magic;
	struct ibv_qp ibv;
	struct ptl_ctx *ctx;
	struct ptl_pd *pd;
	struct ptl_cq *send_cq, *recv_cq;
	struct ptl_srq *srq;
	bool sq_sig_all;     /* every send must complete, acks included */
	uint32_t max_send_wr;
	pthread_mutex_t lock; /* state, peer */
	enum ibv_qp_state state;
	int access;
	enum ibv_mtu mtu;
	bool peer_valid; /* set by RTR */
	struct ptl_peer peer;
};

/* A memory region is bookkeeping plus pinning: the NIC resolves addresses itself
 * (design.md 9.3), so there is no Portals object behind it. */
struct ptl_mr {
	uint32_t magic;
	struct ibv_mr ibv;
	struct ptl_pd *pd;
	bool pinned;
	uintptr_t pstart, pend; /* page-aligned range that was mlocked */
	struct ptl_mr *next;
};

/* Finds a live qp by number (receive path, T3). NULL if there is none. */
struct ptl_qp *ptl_qp_lookup(struct ptl_ctx *ctx, uint32_t qpn);

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

static inline struct ptl_qp *ptl_qp_from_ibv(struct ibv_qp *q)
{
	struct ptl_qp *qp = PTL_CONTAINER_OF(q, struct ptl_qp, ibv);
	if (qp->magic != PTL_MAGIC_QP)
		PTL_FATAL("not a ptl qp: %p", (void *)q);
	return qp;
}

static inline struct ptl_mr *ptl_mr_from_ibv(struct ibv_mr *m)
{
	struct ptl_mr *mr = PTL_CONTAINER_OF(m, struct ptl_mr, ibv);
	if (mr->magic != PTL_MAGIC_MR)
		PTL_FATAL("not a ptl mr: %p", (void *)m);
	return mr;
}

static inline struct ptl_srq *ptl_srq_from_ibv(struct ibv_srq *s)
{
	struct ptl_srq *srq = PTL_CONTAINER_OF(s, struct ptl_srq, ibv);
	if (srq->magic != PTL_MAGIC_SRQ)
		PTL_FATAL("not a ptl srq: %p", (void *)s);
	return srq;
}

#endif /* PTL_INTERNAL_H */
