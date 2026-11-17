/*
 * Completion queues (sprint 1 / T1).
 *
 * A CQ is a logical object: every event of a context arrives on the context's
 * single EQ and poll_cq turns it into work completions. Several CQs can
 * therefore share one context. Until the data path exists (T3) any event on the
 * EQ is unexpected.
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
	free(cq);
	return 0;
}

PTL_EXPORT int ptl_poll_cq(struct ibv_cq *ibv_cq, int num_entries, struct ibv_wc *wc)
{
	struct ptl_cq *cq = ptl_cq_from_ibv(ibv_cq);
	struct ptl_ctx *ctx = cq->ctx;
	ptl_event_t ev;
	int rc;

	(void)wc;
	if (num_entries <= 0)
		return 0;

	pthread_mutex_lock(&ctx->lock);
	rc = PtlEQGet(ctx->eq, &ev);
	pthread_mutex_unlock(&ctx->lock);

	if (rc == PTL_EQ_EMPTY)
		return 0;
	if (rc == PTL_OK || rc == PTL_EQ_DROPPED)
		PTL_FATAL("event type %d (%s) on poll_cq: event handling arrives with the data path (T3)",
			  ev.type, PtlToStr(ev.type, PTL_STR_EVENT));
	PTL_FATAL("PtlEQGet failed: %s", PtlToStr(rc, PTL_STR_ERROR));
}
