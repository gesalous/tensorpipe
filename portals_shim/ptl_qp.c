/*
 * Queue pairs (sprint 2 / test tier T2): creation, the state machine, and the
 * peer address that modify_qp(RTR) learns from the gid. No data moves here.
 *
 * Only reliable-connected QPs on an SRQ are supported: that is all tensorpipe
 * creates. A qpn is 16 bits and unique per context (design.md 9.2).
 */
#include "ptl_internal.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define PTL_MAX_SEND_WR 16384
#define PTL_MAX_SEND_SGE 1

struct ptl_qp *ptl_qp_lookup(struct ptl_ctx *ctx, uint32_t qpn)
{
	struct ptl_qp *qp;

	if (qpn == 0 || qpn > PTL_MAX_QPN)
		return NULL;
	pthread_mutex_lock(&ctx->qp_lock);
	qp = ctx->qps[qpn];
	pthread_mutex_unlock(&ctx->qp_lock);
	return qp;
}

PTL_EXPORT struct ibv_qp *ptl_create_qp(struct ibv_pd *ibv_pd, struct ibv_qp_init_attr *attr)
{
	struct ptl_pd *pd = ptl_pd_from_ibv(ibv_pd);
	struct ptl_ctx *ctx = pd->ctx;
	struct ptl_qp *qp;
	uint32_t qpn, tries;

	if (attr == NULL || attr->send_cq == NULL || attr->recv_cq == NULL) {
		errno = EINVAL;
		return NULL;
	}
	if (attr->qp_type != IBV_QPT_RC) {
		PTL_WARN("only RC queue pairs are supported (asked for type %d)", attr->qp_type);
		errno = EOPNOTSUPP;
		return NULL;
	}
	if (attr->srq == NULL) {
		PTL_WARN("queue pairs without an SRQ are not supported");
		errno = EOPNOTSUPP;
		return NULL;
	}
	if (attr->cap.max_send_wr == 0 || attr->cap.max_send_wr > PTL_MAX_SEND_WR ||
	    attr->cap.max_send_sge > PTL_MAX_SEND_SGE) {
		errno = EINVAL;
		return NULL;
	}
	qp = calloc(1, sizeof(*qp));
	if (qp == NULL) {
		errno = ENOMEM;
		return NULL;
	}
	qp->send_cq = ptl_cq_from_ibv(attr->send_cq);
	qp->recv_cq = ptl_cq_from_ibv(attr->recv_cq);
	qp->srq = ptl_srq_from_ibv(attr->srq);
	if (qp->send_cq->ctx != ctx || qp->recv_cq->ctx != ctx || qp->srq->pd->ctx != ctx) {
		free(qp);
		errno = EINVAL; /* objects of another context */
		return NULL;
	}

	pthread_mutex_lock(&ctx->qp_lock);
	for (tries = 0, qpn = ctx->next_qpn; tries < PTL_MAX_QPN; tries++, qpn++) {
		if (qpn > PTL_MAX_QPN)
			qpn = 1;
		if (ctx->qps[qpn] == NULL)
			break;
	}
	if (tries == PTL_MAX_QPN) {
		pthread_mutex_unlock(&ctx->qp_lock);
		free(qp);
		errno = ENOMEM;
		return NULL;
	}
	ctx->qps[qpn] = qp;
	ctx->next_qpn = qpn + 1;
	pthread_mutex_unlock(&ctx->qp_lock);

	qp->magic = PTL_MAGIC_QP;
	qp->ctx = ctx;
	qp->pd = pd;
	qp->sq_sig_all = attr->sq_sig_all != 0;
	qp->max_send_wr = attr->cap.max_send_wr;
	qp->state = IBV_QPS_RESET;
	pthread_mutex_init(&qp->lock, NULL);
	qp->ibv.context = ibv_pd->context;
	qp->ibv.qp_context = attr->qp_context;
	qp->ibv.pd = ibv_pd;
	qp->ibv.send_cq = attr->send_cq;
	qp->ibv.recv_cq = attr->recv_cq;
	qp->ibv.srq = attr->srq;
	qp->ibv.handle = qpn;
	qp->ibv.qp_num = qpn;
	qp->ibv.state = IBV_QPS_RESET;
	qp->ibv.qp_type = IBV_QPT_RC;
	/* Report what we actually grant. */
	attr->cap.max_send_sge = PTL_MAX_SEND_SGE;
	attr->cap.max_recv_wr = 0;
	attr->cap.max_recv_sge = 0;
	attr->cap.max_inline_data = 0;
	__atomic_fetch_add(&ctx->open_objects, 1, __ATOMIC_RELAXED);
	PTL_DEBUG("qp %u created", qpn);
	return &qp->ibv;
}

/* Attributes the kernel requires for each RC transition. */
#define MASK_INIT (IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)
#define MASK_RTR                                                                              \
	(IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |      \
	 IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER)
#define MASK_RTS                                                                              \
	(IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | \
	 IBV_QP_MAX_QP_RD_ATOMIC)

static int ptl_qp_to_rtr(struct ptl_qp *qp, const struct ibv_qp_attr *attr)
{
	struct ptl_gid g;

	if (!attr->ah_attr.is_global) {
		/* We report lid 0, so a peer is never reachable by lid. */
		PTL_WARN("qp %u: RTR without a global route (dlid %u) is not supported", qp->ibv.qp_num,
			 attr->ah_attr.dlid);
		return EINVAL;
	}
	memcpy(&g, attr->ah_attr.grh.dgid.raw, sizeof(g));
	if (memcmp(g.magic, "PTL4", 4) != 0) {
		PTL_WARN("qp %u: destination gid is not a Portals gid", qp->ibv.qp_num);
		return EINVAL;
	}
	if (attr->dest_qp_num == 0 || attr->dest_qp_num > PTL_MAX_QPN) {
		PTL_WARN("qp %u: destination qpn %u out of range", qp->ibv.qp_num, attr->dest_qp_num);
		return EINVAL;
	}
	if (attr->ah_attr.port_num != 1)
		return EINVAL;
	qp->peer.nid = g.nid;
	qp->peer.pid = g.pid;
	qp->peer.pte = g.pte;
	qp->peer.qpn = attr->dest_qp_num;
	qp->mtu = attr->path_mtu;
	qp->peer_valid = true;
	return 0;
}

PTL_EXPORT int ptl_modify_qp(struct ibv_qp *ibv_qp, struct ibv_qp_attr *attr, int attr_mask)
{
	struct ptl_qp *qp = ptl_qp_from_ibv(ibv_qp);
	enum ibv_qp_state cur, next;
	int rc = 0;

	if (!(attr_mask & IBV_QP_STATE))
		return EINVAL; /* tensorpipe always moves the state; attribute-only changes are unsupported */
	next = attr->qp_state;

	pthread_mutex_lock(&qp->lock);
	cur = qp->state;
	switch (next) {
	case IBV_QPS_INIT:
		if (cur != IBV_QPS_RESET || (attr_mask & MASK_INIT) != MASK_INIT || attr->port_num != 1) {
			rc = EINVAL;
			break;
		}
		qp->access = attr->qp_access_flags;
		break;
	case IBV_QPS_RTR:
		if (cur != IBV_QPS_INIT || (attr_mask & MASK_RTR) != MASK_RTR) {
			rc = EINVAL;
			break;
		}
		rc = ptl_qp_to_rtr(qp, attr);
		break;
	case IBV_QPS_RTS:
		if (cur != IBV_QPS_RTR || (attr_mask & MASK_RTS) != MASK_RTS)
			rc = EINVAL;
		break;
	case IBV_QPS_ERR:
		break; /* legal from any state; flushing outstanding work is T3/T4 */
	case IBV_QPS_RESET:
		qp->peer_valid = false;
		break;
	default:
		rc = EINVAL;
	}
	if (rc != 0) {
		PTL_WARN("qp %u: invalid transition %d -> %d (mask 0x%x), %d", qp->ibv.qp_num, cur, next,
			 attr_mask, rc);
	} else {
		qp->state = next;
		qp->ibv.state = next;
		PTL_DEBUG("qp %u: state %d -> %d", qp->ibv.qp_num, cur, next);
	}
	pthread_mutex_unlock(&qp->lock);
	return rc;
}

PTL_EXPORT int ptl_destroy_qp(struct ibv_qp *ibv_qp)
{
	struct ptl_qp *qp = ptl_qp_from_ibv(ibv_qp);
	struct ptl_ctx *ctx = qp->ctx;

	pthread_mutex_lock(&ctx->qp_lock);
	ctx->qps[qp->ibv.qp_num] = NULL; /* events for this qpn are dropped from now on */
	pthread_mutex_unlock(&ctx->qp_lock);
	__atomic_fetch_sub(&ctx->open_objects, 1, __ATOMIC_RELAXED);
	qp->magic = 0;
	pthread_mutex_destroy(&qp->lock);
	free(qp);
	return 0;
}
