/*
 * Completion queues (sprint 1 / T1).
 *
 * A CQ is a logical object: every event of a context arrives on the context's
 * single EQ; ptl_poll_cq (ptl_data.c) drains it and files each completion in the
 * ring of the CQ it belongs to. Several CQs can therefore share one context.
 */
#include "ptl_internal.h"
#include <errno.h>
#include <stdlib.h>

PTL_EXPORT struct ibv_cq *ptl_create_cq(struct ibv_context *context, int cqe, void *cq_context,
					struct ibv_comp_channel *channel, int comp_vector)
{
	struct ptl_ctx *ctx = ptl_ctx_from_ibv(context);
	struct ptl_cq *cq;

	if (channel != NULL) {
		/* tensorpipe busy-polls and never passes one. */
		PTL_FATAL("completion channels are not supported");
	}
	if (cqe <= 0 || cqe > PTL_EQ_SIZE) {
		errno = EINVAL;
		return NULL;
	}
	cq = calloc(1, sizeof(*cq));
	if (cq == NULL) {
		errno = ENOMEM;
		return NULL;
	}
	cq->cap = 1024;
	cq->ring = calloc(cq->cap, sizeof(*cq->ring));
	if (cq->ring == NULL) {
		free(cq);
		errno = ENOMEM;
		return NULL;
	}
	cq->magic = PTL_MAGIC_CQ;
	cq->ctx = ctx;
	cq->ibv.context = context;
	cq->ibv.cq_context = cq_context;
	cq->ibv.cqe = cqe;
	__atomic_fetch_add(&ctx->open_objects, 1, __ATOMIC_RELAXED);
	return &cq->ibv;
}

PTL_EXPORT int ptl_destroy_cq(struct ibv_cq *ibv_cq)
{
	struct ptl_cq *cq = ptl_cq_from_ibv(ibv_cq);

	__atomic_fetch_sub(&cq->ctx->open_objects, 1, __ATOMIC_RELAXED);
	cq->magic = 0;
	free(cq->ring);
	free(cq);
	return 0;
}

void ptl_cq_push(struct ptl_cq *cq, const struct ibv_wc *wc)
{
	if (cq->count == cq->cap) {
		uint32_t i, ncap = cq->cap * 2;
		struct ibv_wc *n = calloc(ncap, sizeof(*n));
		if (n == NULL)
			PTL_FATAL("out of memory growing a completion queue to %u", ncap);
		for (i = 0; i < cq->count; i++)
			n[i] = cq->ring[(cq->head + i) & (cq->cap - 1)];
		free(cq->ring);
		cq->ring = n;
		cq->cap = ncap;
		cq->head = 0;
	}
	cq->ring[(cq->head + cq->count) & (cq->cap - 1)] = *wc;
	cq->count++;
}
