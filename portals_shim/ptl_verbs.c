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
	PTL_UNIMPLEMENTED();
}

PTL_EXPORT const char *ptl_wc_status_str(enum ibv_wc_status status)
{
	PTL_UNIMPLEMENTED();
}

/* ---- data path --------------------------------------------------------- */

PTL_EXPORT int ptl_post_send(struct ibv_qp *qp, struct ibv_send_wr *wr, struct ibv_send_wr **bad_wr)
{
	PTL_UNIMPLEMENTED();
}

PTL_EXPORT int ptl_post_recv(struct ibv_qp *qp, struct ibv_recv_wr *wr, struct ibv_recv_wr **bad_wr)
{
	PTL_UNIMPLEMENTED();
}
