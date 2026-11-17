/*
 * Sprint 0 of the tensorpipe-over-Portals4 port.
 *
 * Exports the subset of the verbs API that tensorpipe's ibv transport uses,
 * under a "ptl_" prefix and with the signatures of the ibv_* originals. For
 * now every function forwards to the real libibverbs, so selecting this
 * library (TP_IBV_LIBRARY=/path/to/libtensorpipe_portals.so) must not change
 * behaviour at all. Later sprints replace the bodies with Portals4 code, one
 * function at a time.
 *
 * Loaded by tensorpipe through dlopen(RTLD_LOCAL) and dlsym only: nothing
 * here is visible to other verbs users in the same process.
 */
#include <infiniband/verbs.h>
#include <stdio.h>
#include <stdlib.h>

/* Data-path call counters, reported at exit when TP_PTL_TRACE is set. */
static unsigned long ptl_n_poll_cq, ptl_n_post_send, ptl_n_post_recv,
		     ptl_n_post_srq_recv;

#define PTL_EXPORT __attribute__((visibility("default")))

/* TP_PTL_TRACE=1 logs every call, to confirm the shim really is in use. */
static int ptl_trace_enabled(void)
{
	static int v = -1;
	if (v < 0) {
		const char *e = getenv("TP_PTL_TRACE");
		v = (e != NULL && e[0] != '\0' && e[0] != '0');
	}
	return v;
}
static void ptl_report_counters(void)
{
	fprintf(stderr,
		"[ptl-shim] data path calls: poll_cq=%lu post_send=%lu post_recv=%lu post_srq_recv=%lu\n",
		ptl_n_poll_cq, ptl_n_post_send, ptl_n_post_recv, ptl_n_post_srq_recv);
}

__attribute__((constructor)) static void ptl_shim_init(void)
{
	if (ptl_trace_enabled())
		atexit(ptl_report_counters);
}

#define PTL_TRACE(name) \
	do { if (ptl_trace_enabled()) fprintf(stderr, "[ptl-shim] %s\n", name); } while (0)

/* ---- control path ------------------------------------------------------ */

PTL_EXPORT struct ibv_device **ptl_get_device_list(int *num_devices)
{
	PTL_TRACE("get_device_list");
	return ibv_get_device_list(num_devices);
}

PTL_EXPORT void ptl_free_device_list(struct ibv_device **list)
{
	PTL_TRACE("free_device_list");
	ibv_free_device_list(list);
}

PTL_EXPORT const char *ptl_get_device_name(struct ibv_device *device)
{
	PTL_TRACE("get_device_name");
	return ibv_get_device_name(device);
}

PTL_EXPORT struct ibv_context *ptl_open_device(struct ibv_device *device)
{
	PTL_TRACE("open_device");
	return ibv_open_device(device);
}

PTL_EXPORT int ptl_close_device(struct ibv_context *context)
{
	PTL_TRACE("close_device");
	return ibv_close_device(context);
}

PTL_EXPORT int ptl_query_port(struct ibv_context *context, uint8_t port_num,
			      struct ibv_port_attr *port_attr)
{
	PTL_TRACE("query_port");
	return ibv_query_port(context, port_num, port_attr);
}

PTL_EXPORT int ptl_query_gid(struct ibv_context *context, uint8_t port_num,
			     int index, union ibv_gid *gid)
{
	PTL_TRACE("query_gid");
	return ibv_query_gid(context, port_num, index, gid);
}

PTL_EXPORT struct ibv_pd *ptl_alloc_pd(struct ibv_context *context)
{
	PTL_TRACE("alloc_pd");
	return ibv_alloc_pd(context);
}

PTL_EXPORT int ptl_dealloc_pd(struct ibv_pd *pd)
{
	PTL_TRACE("dealloc_pd");
	return ibv_dealloc_pd(pd);
}

PTL_EXPORT struct ibv_mr *ptl_reg_mr(struct ibv_pd *pd, void *addr, size_t length,
				     int access)
{
	PTL_TRACE("reg_mr");
	return ibv_reg_mr(pd, addr, length, access);
}

PTL_EXPORT int ptl_dereg_mr(struct ibv_mr *mr)
{
	PTL_TRACE("dereg_mr");
	return ibv_dereg_mr(mr);
}

PTL_EXPORT struct ibv_cq *ptl_create_cq(struct ibv_context *context, int cqe,
					void *cq_context,
					struct ibv_comp_channel *channel,
					int comp_vector)
{
	PTL_TRACE("create_cq");
	return ibv_create_cq(context, cqe, cq_context, channel, comp_vector);
}

PTL_EXPORT int ptl_destroy_cq(struct ibv_cq *cq)
{
	PTL_TRACE("destroy_cq");
	return ibv_destroy_cq(cq);
}

PTL_EXPORT struct ibv_srq *ptl_create_srq(struct ibv_pd *pd,
					  struct ibv_srq_init_attr *attr)
{
	PTL_TRACE("create_srq");
	return ibv_create_srq(pd, attr);
}

PTL_EXPORT int ptl_destroy_srq(struct ibv_srq *srq)
{
	PTL_TRACE("destroy_srq");
	return ibv_destroy_srq(srq);
}

PTL_EXPORT struct ibv_qp *ptl_create_qp(struct ibv_pd *pd,
					struct ibv_qp_init_attr *attr)
{
	PTL_TRACE("create_qp");
	return ibv_create_qp(pd, attr);
}

PTL_EXPORT int ptl_modify_qp(struct ibv_qp *qp, struct ibv_qp_attr *attr,
			     int attr_mask)
{
	PTL_TRACE("modify_qp");
	return ibv_modify_qp(qp, attr, attr_mask);
}

PTL_EXPORT int ptl_destroy_qp(struct ibv_qp *qp)
{
	PTL_TRACE("destroy_qp");
	return ibv_destroy_qp(qp);
}

PTL_EXPORT int ptl_get_async_event(struct ibv_context *context,
				   struct ibv_async_event *event)
{
	PTL_TRACE("get_async_event");
	return ibv_get_async_event(context, event);
}

PTL_EXPORT void ptl_ack_async_event(struct ibv_async_event *event)
{
	PTL_TRACE("ack_async_event");
	ibv_ack_async_event(event);
}

PTL_EXPORT const char *ptl_event_type_str(enum ibv_event_type event)
{
	return ibv_event_type_str(event);
}

PTL_EXPORT const char *ptl_wc_status_str(enum ibv_wc_status status)
{
	return ibv_wc_status_str(status);
}

/* ---- data path ----------------------------------------------------------
 * These are inline in <infiniband/verbs.h> (they go through ctx->ops), so
 * tensorpipe cannot dlsym them from libibverbs. Exporting them here is what
 * lets a Portals-backed library intercept them.  Not traced: hot path.
 */

PTL_EXPORT int ptl_poll_cq(struct ibv_cq *cq, int num_entries, struct ibv_wc *wc)
{
	__atomic_fetch_add(&ptl_n_poll_cq, 1, __ATOMIC_RELAXED);
	return ibv_poll_cq(cq, num_entries, wc);
}

PTL_EXPORT int ptl_post_send(struct ibv_qp *qp, struct ibv_send_wr *wr,
			     struct ibv_send_wr **bad_wr)
{
	__atomic_fetch_add(&ptl_n_post_send, 1, __ATOMIC_RELAXED);
	return ibv_post_send(qp, wr, bad_wr);
}

PTL_EXPORT int ptl_post_recv(struct ibv_qp *qp, struct ibv_recv_wr *wr,
			     struct ibv_recv_wr **bad_wr)
{
	__atomic_fetch_add(&ptl_n_post_recv, 1, __ATOMIC_RELAXED);
	return ibv_post_recv(qp, wr, bad_wr);
}

PTL_EXPORT int ptl_post_srq_recv(struct ibv_srq *srq, struct ibv_recv_wr *wr,
				 struct ibv_recv_wr **bad_wr)
{
	__atomic_fetch_add(&ptl_n_post_srq_recv, 1, __ATOMIC_RELAXED);
	return ibv_post_srq_recv(srq, wr, bad_wr);
}
