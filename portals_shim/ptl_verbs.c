/*
 * tensorpipe-over-Portals4: verbs shim skeleton.
 *
 * Exports the subset of the verbs API that tensorpipe's ibv transport uses,
 * under a "ptl_" prefix and with the signatures of the ibv_* originals. Every
 * function is currently a stub that aborts with "unimplemented": they get
 * implemented one at a time, in the order given by the test ladder in
 * design.md (section 9), so a test that reaches an unimplemented function
 * fails loudly and names the function.
 *
 * This library must not link libibverbs and must never call ibv_*: tensorpipe
 * opens it with dlopen(RTLD_LOCAL) and resolves ptl_* with dlsym, so it stays
 * invisible to other verbs users in the same process.
 */
#include <infiniband/verbs.h> /* types only */
#include <stdio.h>
#include <stdlib.h>

#define PTL_EXPORT __attribute__((visibility("default")))

#define PTL_UNIMPLEMENTED()                                                    \
	do {                                                                   \
		fprintf(stderr, "[ptl-shim] FATAL: %s is unimplemented (%s:%d)\n",  \
			__func__, __FILE__, __LINE__);                         \
		abort();                                                       \
	} while (0)

/* ---- control path ------------------------------------------------------ */

PTL_EXPORT struct ibv_device **ptl_get_device_list(int *num_devices)
{
	PTL_UNIMPLEMENTED();
}

PTL_EXPORT void ptl_free_device_list(struct ibv_device **list)
{
	PTL_UNIMPLEMENTED();
}

PTL_EXPORT const char *ptl_get_device_name(struct ibv_device *device)
{
	PTL_UNIMPLEMENTED();
}

PTL_EXPORT struct ibv_context *ptl_open_device(struct ibv_device *device)
{
	PTL_UNIMPLEMENTED();
}

PTL_EXPORT int ptl_close_device(struct ibv_context *context)
{
	PTL_UNIMPLEMENTED();
}

PTL_EXPORT int ptl_query_port(struct ibv_context *context, uint8_t port_num, struct ibv_port_attr *port_attr)
{
	PTL_UNIMPLEMENTED();
}

PTL_EXPORT int ptl_query_gid(struct ibv_context *context, uint8_t port_num, int index, union ibv_gid *gid)
{
	PTL_UNIMPLEMENTED();
}

PTL_EXPORT struct ibv_pd *ptl_alloc_pd(struct ibv_context *context)
{
	PTL_UNIMPLEMENTED();
}

PTL_EXPORT int ptl_dealloc_pd(struct ibv_pd *pd)
{
	PTL_UNIMPLEMENTED();
}

PTL_EXPORT struct ibv_mr *ptl_reg_mr(struct ibv_pd *pd, void *addr, size_t length, int access)
{
	PTL_UNIMPLEMENTED();
}

PTL_EXPORT int ptl_dereg_mr(struct ibv_mr *mr)
{
	PTL_UNIMPLEMENTED();
}

PTL_EXPORT struct ibv_cq *ptl_create_cq(struct ibv_context *context, int cqe, void *cq_context, struct ibv_comp_channel *channel, int comp_vector)
{
	PTL_UNIMPLEMENTED();
}

PTL_EXPORT int ptl_destroy_cq(struct ibv_cq *cq)
{
	PTL_UNIMPLEMENTED();
}

PTL_EXPORT struct ibv_srq *ptl_create_srq(struct ibv_pd *pd, struct ibv_srq_init_attr *attr)
{
	PTL_UNIMPLEMENTED();
}

PTL_EXPORT int ptl_destroy_srq(struct ibv_srq *srq)
{
	PTL_UNIMPLEMENTED();
}

PTL_EXPORT struct ibv_qp *ptl_create_qp(struct ibv_pd *pd, struct ibv_qp_init_attr *attr)
{
	PTL_UNIMPLEMENTED();
}

PTL_EXPORT int ptl_modify_qp(struct ibv_qp *qp, struct ibv_qp_attr *attr, int attr_mask)
{
	PTL_UNIMPLEMENTED();
}

PTL_EXPORT int ptl_destroy_qp(struct ibv_qp *qp)
{
	PTL_UNIMPLEMENTED();
}

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

PTL_EXPORT int ptl_poll_cq(struct ibv_cq *cq, int num_entries, struct ibv_wc *wc)
{
	PTL_UNIMPLEMENTED();
}

PTL_EXPORT int ptl_post_send(struct ibv_qp *qp, struct ibv_send_wr *wr, struct ibv_send_wr **bad_wr)
{
	PTL_UNIMPLEMENTED();
}

PTL_EXPORT int ptl_post_recv(struct ibv_qp *qp, struct ibv_recv_wr *wr, struct ibv_recv_wr **bad_wr)
{
	PTL_UNIMPLEMENTED();
}

PTL_EXPORT int ptl_post_srq_recv(struct ibv_srq *srq, struct ibv_recv_wr *wr, struct ibv_recv_wr **bad_wr)
{
	PTL_UNIMPLEMENTED();
}
