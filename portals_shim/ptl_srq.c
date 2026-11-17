/*
 * Shared receive queue (sprint 1 / T1). Accounting only: see ptl_internal.h.
 */
#include "ptl_internal.h"
#include <errno.h>
#include <stdlib.h>

PTL_EXPORT struct ibv_srq *ptl_create_srq(struct ibv_pd *ibv_pd, struct ibv_srq_init_attr *attr)
{
	struct ptl_pd *pd = ptl_pd_from_ibv(ibv_pd);
	struct ptl_srq *srq;

	if (attr == NULL || attr->attr.max_wr == 0) {
		errno = EINVAL;
		return NULL;
	}
	srq = calloc(1, sizeof(*srq));
	if (srq == NULL) {
		errno = ENOMEM;
		return NULL;
	}
	srq->wr_ids = calloc(attr->attr.max_wr, sizeof(*srq->wr_ids));
	if (srq->wr_ids == NULL) {
		free(srq);
		errno = ENOMEM;
		return NULL;
	}
	srq->rx_cap = 1024;
	srq->rx = calloc(srq->rx_cap, sizeof(*srq->rx));
	if (srq->rx == NULL) {
		free(srq->wr_ids);
		free(srq);
		errno = ENOMEM;
		return NULL;
	}
	srq->magic = PTL_MAGIC_SRQ;
	srq->pd = pd;
	srq->cap = attr->attr.max_wr;
	pthread_mutex_init(&srq->lock, NULL);
	srq->ibv.context = pd->ibv.context;
	srq->ibv.srq_context = attr->srq_context;
	srq->ibv.pd = ibv_pd;
	pthread_mutex_lock(&pd->ctx->lock);
	srq->next = pd->ctx->srqs;
	pd->ctx->srqs = srq;
	pthread_mutex_unlock(&pd->ctx->lock);
	__atomic_fetch_add(&pd->ctx->open_objects, 1, __ATOMIC_RELAXED);
	return &srq->ibv;
}

PTL_EXPORT int ptl_destroy_srq(struct ibv_srq *ibv_srq)
{
	struct ptl_srq *srq = ptl_srq_from_ibv(ibv_srq);
	struct ptl_ctx *ctx = srq->pd->ctx;
	struct ptl_srq **pp;

	pthread_mutex_lock(&ctx->lock);
	for (pp = &ctx->srqs; *pp != NULL && *pp != srq; pp = &(*pp)->next)
		;
	if (*pp != NULL)
		*pp = srq->next;
	pthread_mutex_unlock(&ctx->lock);
	__atomic_fetch_sub(&ctx->open_objects, 1, __ATOMIC_RELAXED);
	srq->magic = 0;
	free(srq->rx);
	pthread_mutex_destroy(&srq->lock);
	free(srq->wr_ids);
	free(srq);
	return 0;
}

PTL_EXPORT int ptl_post_srq_recv(struct ibv_srq *ibv_srq, struct ibv_recv_wr *wr,
				 struct ibv_recv_wr **bad_wr)
{
	struct ptl_srq *srq = ptl_srq_from_ibv(ibv_srq);
	int rc = 0;

	pthread_mutex_lock(&srq->lock);
	for (; wr != NULL; wr = wr->next) {
		if (wr->num_sge != 0) {
			/* Only zero-length receives (tensorpipe's) are supported: data
			 * lands through the LE, not through a posted buffer. */
			PTL_FATAL("post_srq_recv with %d sges is not supported", wr->num_sge);
		}
		if (srq->count == srq->cap) {
			*bad_wr = wr;
			rc = ENOMEM;
			break;
		}
		srq->wr_ids[(srq->head + srq->count) % srq->cap] = wr->wr_id;
		srq->count++;
	}
	pthread_mutex_unlock(&srq->lock);
	return rc;
}

void ptl_srq_push_rx(struct ptl_srq *srq, const struct ptl_rx *rx)
{
	if (srq->rx_count == srq->rx_cap) {
		uint32_t i, ncap = srq->rx_cap * 2;
		struct ptl_rx *n = calloc(ncap, sizeof(*n));
		if (n == NULL)
			PTL_FATAL("out of memory growing the receive backlog to %u", ncap);
		for (i = 0; i < srq->rx_count; i++)
			n[i] = srq->rx[(srq->rx_head + i) & (srq->rx_cap - 1)];
		free(srq->rx);
		srq->rx = n;
		srq->rx_cap = ncap;
		srq->rx_head = 0;
	}
	srq->rx[(srq->rx_head + srq->rx_count) & (srq->rx_cap - 1)] = *rx;
	srq->rx_count++;
}
