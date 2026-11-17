/*
 * The data path (sprint 3 / test tier T3): post_send, event handling, poll_cq.
 *
 * Sends. RDMA_WRITE_WITH_IMM becomes a PtlPut of the sge to remote_offset =
 * remote_addr; SEND_WITH_IMM (zero length, as tensorpipe uses it for acks) becomes
 * a zero-length PtlPut. Both go to the peer's single PTE and its persistent LE.
 * hdr_data carries kind, destination qpn, imm and a sequence number (design.md 5.5).
 *
 * Receives. Each PTL_EVENT_PUT on our PTE is a message for one of our QPs. Data
 * writes are delivered in sequence order (Portals does not order them): early ones
 * wait in the QP's reorder buffer. A delivered message needs a receive wr_id from the
 * SRQ; if none is posted yet it waits in the SRQ's backlog. Acks are additive and
 * need no ordering.
 *
 * Send completions come from PTL_EVENT_ACK (the target has the data), one per post,
 * possibly out of posting order.
 *
 * Locking: ctx->lock guards PtlPut, the EQ, the CQ rings, backlogs and reorder
 * state. Order: qp->lock, then ctx->lock, then qp_lock or srq->lock.
 */
#include "ptl_internal.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define PTL_EVENT_BATCH 256 /* events drained per poll_cq call */
#define PTL_SEQ_WINDOW (1U << (PTL_SEQ_BITS - 1))
#define REORDER_VALID (1ULL << 63) /* | mlength << 32 | imm */

struct ptl_tx { /* one per posted send; handed back as the event's user_ptr */
	uint32_t qpn;
	uint32_t opcode; /* enum ibv_wc_opcode */
	uint64_t wr_id;
	uint32_t len;
};

/* ---- sending ------------------------------------------------------------ */

static int post_one(struct ptl_qp *qp, const struct ibv_send_wr *wr)
{
	struct ptl_ctx *ctx = qp->ctx;
	struct ptl_tx *tx;
	ptl_process_t target;
	uint64_t hdr, local = 0, remote = 0;
	uint32_t len = 0, seq = 0;
	bool data;
	int rc;

	if (qp->state != IBV_QPS_RTS || !qp->peer_valid) {
		PTL_WARN("qp %u: post_send in state %d", qp->ibv.qp_num, qp->state);
		return EINVAL;
	}
	if (__atomic_load_n(&qp->inflight, __ATOMIC_RELAXED) >= (int)qp->max_send_wr)
		return ENOMEM;

	switch (wr->opcode) {
	case IBV_WR_RDMA_WRITE_WITH_IMM:
		if (wr->num_sge != 1) {
			PTL_WARN("RDMA write with %d sges is not supported", wr->num_sge);
			return EINVAL;
		}
		if (!ptl_mr_covers(wr->sg_list[0].lkey, wr->sg_list[0].addr, wr->sg_list[0].length)) {
			PTL_WARN("qp %u: sge [%p, +%u) is not inside the region with lkey %u", qp->ibv.qp_num,
				 (void *)(uintptr_t)wr->sg_list[0].addr, wr->sg_list[0].length, wr->sg_list[0].lkey);
			return EFAULT;
		}
		data = true;
		local = wr->sg_list[0].addr;
		len = wr->sg_list[0].length;
		remote = wr->wr.rdma.remote_addr;
		break;
	case IBV_WR_SEND_WITH_IMM:
		if (wr->num_sge != 0) {
			PTL_WARN("send with %d sges is not supported (only zero-length acks)", wr->num_sge);
			return EINVAL;
		}
		data = false;
		break;
	default:
		PTL_WARN("qp %u: unsupported send opcode %d", qp->ibv.qp_num, wr->opcode);
		return EINVAL;
	}

	tx = malloc(sizeof(*tx));
	if (tx == NULL)
		return ENOMEM;
	tx->qpn = qp->ibv.qp_num;
	tx->opcode = data ? IBV_WC_RDMA_WRITE : IBV_WC_SEND;
	tx->wr_id = wr->wr_id;
	tx->len = len;

	if (data)
		seq = qp->tx_seq++ & PTL_SEQ_MASK;
	hdr = PTL_HDR(data ? PTL_KIND_DATA : PTL_KIND_ACK, qp->peer.qpn, wr->imm_data, seq);
	target.phys.nid = qp->peer.nid;
	target.phys.pid = qp->peer.pid;

	__atomic_fetch_add(&qp->inflight, 1, __ATOMIC_RELAXED);
	pthread_mutex_lock(&ctx->lock);
	rc = PtlPut(ctx->md, (ptl_size_t)local, len, PTL_ACK_REQ, target, qp->peer.pte, 0,
		    (ptl_size_t)remote, tx, hdr);
	pthread_mutex_unlock(&ctx->lock);
	if (rc != PTL_OK) {
		PTL_WARN("qp %u: PtlPut failed: %s", qp->ibv.qp_num, PtlToStr(rc, PTL_STR_ERROR));
		__atomic_fetch_sub(&qp->inflight, 1, __ATOMIC_RELAXED);
		if (data)
			qp->tx_seq--; /* the number was never sent */
		free(tx);
		return rc == PTL_NO_SPACE ? ENOMEM : EIO;
	}
	return 0;
}

PTL_EXPORT int ptl_post_send(struct ibv_qp *ibv_qp, struct ibv_send_wr *wr, struct ibv_send_wr **bad_wr)
{
	struct ptl_qp *qp = ptl_qp_from_ibv(ibv_qp);
	int rc = 0;

	pthread_mutex_lock(&qp->lock);
	for (; wr != NULL; wr = wr->next) {
		rc = post_one(qp, wr);
		if (rc != 0) {
			*bad_wr = wr;
			break;
		}
	}
	pthread_mutex_unlock(&qp->lock);
	return rc;
}

/* ---- events -> completions (ctx->lock held) ------------------------------- */

static void rx_deliver(struct ptl_qp *qp, uint32_t opcode, uint32_t imm, uint32_t len)
{
	struct ptl_rx rx = {.qpn = qp->ibv.qp_num, .opcode = opcode, .imm = imm, .byte_len = len};

	ptl_srq_push_rx(qp->srq, &rx);
}

static void handle_data(struct ptl_qp *qp, uint32_t seq, uint32_t imm, uint32_t len)
{
	uint32_t dist = (seq - qp->rx_next) & PTL_SEQ_MASK;

	if (dist >= PTL_SEQ_WINDOW)
		PTL_FATAL("qp %u: data sequence %u is behind the expected %u (duplicate?)", qp->ibv.qp_num, seq,
			  qp->rx_next);
	if (dist != 0) {
		if (qp->reorder == NULL) {
			qp->reorder = calloc(PTL_SEQ_MASK + 1, sizeof(*qp->reorder));
			if (qp->reorder == NULL)
				PTL_FATAL("out of memory allocating a reorder buffer");
		}
		PTL_DEBUG("qp %u: data seq %u arrived early (expected %u)", qp->ibv.qp_num, seq, qp->rx_next);
		qp->reorder[seq] = REORDER_VALID | ((uint64_t)len << 32) | imm;
		return;
	}
	rx_deliver(qp, IBV_WC_RECV_RDMA_WITH_IMM, imm, len);
	qp->rx_next = (qp->rx_next + 1) & PTL_SEQ_MASK;
	while (qp->reorder != NULL && (qp->reorder[qp->rx_next] & REORDER_VALID)) {
		uint64_t e = qp->reorder[qp->rx_next];
		qp->reorder[qp->rx_next] = 0;
		rx_deliver(qp, IBV_WC_RECV_RDMA_WITH_IMM, (uint32_t)e, (uint32_t)((e >> 32) & 0x7fffffff));
		qp->rx_next = (qp->rx_next + 1) & PTL_SEQ_MASK;
	}
}

static void handle_event(struct ptl_ctx *ctx, const ptl_event_t *ev)
{
	switch (ev->type) {
	case PTL_EVENT_SEND:
		if (ev->ni_fail_type != PTL_NI_OK)
			PTL_WARN("send failed locally: %s", PtlToStr(ev->ni_fail_type, PTL_STR_FAIL_TYPE));
		return;
	case PTL_EVENT_ACK: {
		struct ptl_tx *tx = ev->user_ptr;
		struct ptl_qp *qp = ptl_qp_lookup(ctx, tx->qpn);
		struct ibv_wc wc;

		if (qp != NULL) {
			memset(&wc, 0, sizeof(wc));
			wc.wr_id = tx->wr_id;
			wc.opcode = tx->opcode;
			wc.byte_len = tx->len;
			wc.qp_num = tx->qpn;
			wc.status = IBV_WC_SUCCESS;
			if (ev->ni_fail_type != PTL_NI_OK) {
				PTL_WARN("qp %u: send was not delivered: %s", tx->qpn,
					 PtlToStr(ev->ni_fail_type, PTL_STR_FAIL_TYPE));
				wc.status = IBV_WC_RETRY_EXC_ERR;
			}
			ptl_cq_push(qp->send_cq, &wc);
			__atomic_fetch_sub(&qp->inflight, 1, __ATOMIC_RELAXED);
		}
		free(tx);
		return;
	}
	case PTL_EVENT_PUT: {
		uint64_t h = ev->hdr_data;
		uint32_t qpn = (h >> 46) & 0xffff, imm = (h >> 14) & 0xffffffffU, seq = h & PTL_SEQ_MASK;
		struct ptl_qp *qp = ptl_qp_lookup(ctx, qpn);

		if (ev->ni_fail_type != PTL_NI_OK) {
			PTL_WARN("incoming put failed: %s", PtlToStr(ev->ni_fail_type, PTL_STR_FAIL_TYPE));
			return;
		}
		if (qp == NULL) {
			PTL_WARN("put for unknown qp %u dropped", qpn);
			return;
		}
		if ((h >> 62) == PTL_KIND_ACK)
			rx_deliver(qp, IBV_WC_RECV, imm, 0);
		else
			handle_data(qp, seq, imm, (uint32_t)ev->mlength);
		return;
	}
	default:
		PTL_DEBUG("ignoring event %s", PtlToStr(ev->type, PTL_STR_EVENT));
	}
}

/* Give backlogged receives their wr_ids, in arrival order. */
static void match_receives(struct ptl_ctx *ctx)
{
	struct ptl_srq *srq;

	for (srq = ctx->srqs; srq != NULL; srq = srq->next) {
		pthread_mutex_lock(&srq->lock);
		while (srq->rx_count > 0 && srq->count > 0) {
			const struct ptl_rx *rx = &srq->rx[srq->rx_head];
			struct ptl_qp *qp = ptl_qp_lookup(ctx, rx->qpn);
			uint64_t wr_id = srq->wr_ids[srq->head];
			struct ibv_wc wc;

			srq->head = (srq->head + 1) % srq->cap;
			srq->count--;
			if (qp != NULL) {
				memset(&wc, 0, sizeof(wc));
				wc.wr_id = wr_id;
				wc.status = IBV_WC_SUCCESS;
				wc.opcode = rx->opcode;
				wc.byte_len = rx->byte_len;
				wc.imm_data = rx->imm;
				wc.wc_flags = IBV_WC_WITH_IMM;
				wc.qp_num = rx->qpn;
				wc.src_qp = qp->peer.qpn;
				ptl_cq_push(qp->recv_cq, &wc);
			}
			srq->rx_head = (srq->rx_head + 1) & (srq->rx_cap - 1);
			srq->rx_count--;
		}
		pthread_mutex_unlock(&srq->lock);
	}
}

/* Test aid, TP_PTL_TEST_SWAP=1: deliver data writes pairwise swapped, as if the network
 * had reordered them, to exercise the reorder buffer on a network that does not. */
static bool test_swap_enabled(void)
{
	static int v = -1;
	if (v < 0) {
		const char *e = getenv("TP_PTL_TEST_SWAP");
		v = (e != NULL && e[0] == '1') ? 1 : 0;
	}
	return v == 1;
}

static void handle_event_swapping(struct ptl_ctx *ctx, const ptl_event_t *ev)
{
	if (!test_swap_enabled() || ev->type != PTL_EVENT_PUT || (ev->hdr_data >> 62) != PTL_KIND_DATA) {
		handle_event(ctx, ev);
		return;
	}
	if (!ctx->has_held) {
		ctx->held = *ev;
		ctx->has_held = true;
		ctx->held_polls = 0;
		return;
	}
	handle_event(ctx, ev);
	handle_event(ctx, &ctx->held);
	ctx->has_held = false;
}

PTL_EXPORT int ptl_poll_cq(struct ibv_cq *ibv_cq, int num_entries, struct ibv_wc *wc)
{
	struct ptl_cq *cq = ptl_cq_from_ibv(ibv_cq);
	struct ptl_ctx *ctx = cq->ctx;
	ptl_event_t ev;
	int i, n = 0;

	if (num_entries <= 0)
		return 0;
	pthread_mutex_lock(&ctx->lock);
	for (i = 0; i < PTL_EVENT_BATCH; i++) {
		int rc = PtlEQGet(ctx->eq, &ev);
		if (rc == PTL_EQ_EMPTY)
			break;
		if (rc != PTL_OK)
			PTL_FATAL("PtlEQGet: %s (the event queue overflowed?)", PtlToStr(rc, PTL_STR_ERROR));
		handle_event_swapping(ctx, &ev);
	}
	if (ctx->has_held && ++ctx->held_polls > 2000) { /* nothing came to swap it with */
		handle_event(ctx, &ctx->held);
		ctx->has_held = false;
	}
	match_receives(ctx);
	while (n < num_entries && cq->count > 0) {
		wc[n++] = cq->ring[cq->head];
		cq->head = (cq->head + 1) & (cq->cap - 1);
		cq->count--;
	}
	pthread_mutex_unlock(&ctx->lock);
	return n;
}
