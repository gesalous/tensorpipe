/*
 * tensorpipe-over-Portals4: verbs shim skeleton.
 *
 * Exports the subset of the verbs API that tensorpipe's ibv transport uses,
 * under a "ptl_" prefix and with the signatures of the ibv_* originals. Functions
 * not yet implemented are stubs that abort with "unimplemented"; they get
 * implemented in the order given by the test ladder in design.md (section 9),
 * so a test that reaches one fails loudly and names it. Implemented ones live
 * in ptl_device.c (device, context, pd, port, gid), ptl_cq.c, ptl_srq.c, ptl_mr.c
 * and ptl_qp.c.
 *
 * This library must not link libibverbs and must never call ibv_*: tensorpipe
 * opens it with dlopen(RTLD_LOCAL) and resolves ptl_* with dlsym, so it stays
 * invisible to other verbs users in the same process.
 */
#include "ptl_internal.h"

/* ---- control path ------------------------------------------------------ */

PTL_EXPORT int ptl_get_async_event(struct ibv_context *context, struct ibv_async_event *event)
{
	PTL_UNIMPLEMENTED();
}

PTL_EXPORT void ptl_ack_async_event(struct ibv_async_event *event)
{
	PTL_UNIMPLEMENTED();
}

PTL_EXPORT const char *ptl_event_type_str(enum ibv_event_type event)
{
	switch (event) {
	case IBV_EVENT_QP_FATAL: return "local work queue catastrophic error";
	case IBV_EVENT_QP_REQ_ERR: return "invalid request local work queue error";
	case IBV_EVENT_QP_ACCESS_ERR: return "local access violation work queue error";
	case IBV_EVENT_COMM_EST: return "communication established";
	case IBV_EVENT_SQ_DRAINED: return "send queue drained";
	case IBV_EVENT_PATH_MIG: return "path migrated";
	case IBV_EVENT_CQ_ERR: return "CQ error";
	case IBV_EVENT_DEVICE_FATAL: return "local catastrophic error";
	case IBV_EVENT_PORT_ACTIVE: return "port active";
	case IBV_EVENT_PORT_ERR: return "port error";
	default: return "unknown event";
	}
}

PTL_EXPORT const char *ptl_wc_status_str(enum ibv_wc_status status)
{
	switch (status) {
	case IBV_WC_SUCCESS: return "success";
	case IBV_WC_LOC_LEN_ERR: return "local length error";
	case IBV_WC_LOC_QP_OP_ERR: return "local QP operation error";
	case IBV_WC_LOC_PROT_ERR: return "local protection error";
	case IBV_WC_WR_FLUSH_ERR: return "Work Request Flushed Error";
	case IBV_WC_REM_ACCESS_ERR: return "remote access error";
	case IBV_WC_RETRY_EXC_ERR: return "transport retry counter exceeded";
	case IBV_WC_RNR_RETRY_EXC_ERR: return "RNR retry counter exceeded";
	case IBV_WC_GENERAL_ERR: return "general error";
	default: return "unknown status";
	}
}

/* post_send and poll_cq are in ptl_data.c. */
PTL_EXPORT int ptl_post_recv(struct ibv_qp *qp, struct ibv_recv_wr *wr, struct ibv_recv_wr **bad_wr)
{
	PTL_UNIMPLEMENTED(); /* tensorpipe receives only through the SRQ */
}
