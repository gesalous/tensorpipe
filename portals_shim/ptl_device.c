/*
 * Device, context, protection domain, port and gid (sprint 1 / test tier T1).
 *
 * One fake device. open_device builds, per context, the Portals resources every
 * connection in it shares:
 *   - one EQ receiving every event of the context (CQs are logical, see ptl_cq.c)
 *   - one whole-address-space MD, so PtlPut/PtlGet can send from any buffer
 *   - one PTE bound to that EQ, with one persistent whole-address-space LE.
 *     Peers' RDMA writes and sends both land on it; hdr_data tells them apart
 *     (design.md 5.4). Whole address space means no remote protection (5.3).
 */
#include "ptl_internal.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct ibv_device g_device = {
	.node_type = IBV_NODE_UNKNOWN,
	.transport_type = IBV_TRANSPORT_UNKNOWN,
	.name = "portals0",
	.dev_name = "portals0",
	.dev_path = "/dev/portals0",
	.ibdev_path = "/sys/class/infiniband/portals0",
};

PTL_EXPORT struct ibv_device **ptl_get_device_list(int *num_devices)
{
	struct ibv_device **list;
	int n = (ptl_ni_get() != NULL) ? 1 : 0; /* no usable BXI NI: no device */

	list = calloc((size_t)n + 1, sizeof(*list));
	if (list == NULL) {
		errno = ENOMEM;
		return NULL;
	}
	if (n == 1)
		list[0] = &g_device;
	if (num_devices != NULL)
		*num_devices = n;
	return list;
}

PTL_EXPORT void ptl_free_device_list(struct ibv_device **list)
{
	free(list);
}

PTL_EXPORT const char *ptl_get_device_name(struct ibv_device *device)
{
	return device->name;
}

PTL_EXPORT struct ibv_context *ptl_open_device(struct ibv_device *device)
{
	const struct ptl_ni *ni;
	struct ptl_ctx *ctx;
	ptl_md_t md;
	ptl_le_t le;
	int rc;

	if (device != &g_device) {
		errno = ENODEV;
		return NULL;
	}
	ni = ptl_ni_get();
	if (ni == NULL)
		return NULL; /* errno set */

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		errno = ENOMEM;
		return NULL;
	}
	ctx->magic = PTL_MAGIC_CTX;
	ctx->ni = ni;
	pthread_mutex_init(&ctx->lock, NULL);
	pthread_mutex_init(&ctx->qp_lock, NULL);
	ctx->next_qpn = 1;
	ctx->qps = calloc((size_t)PTL_MAX_QPN + 1, sizeof(*ctx->qps));
	if (ctx->qps == NULL) {
		pthread_mutex_destroy(&ctx->lock);
		pthread_mutex_destroy(&ctx->qp_lock);
		free(ctx);
		errno = ENOMEM;
		return NULL;
	}
	ctx->ibv.device = &g_device;
	ctx->ibv.cmd_fd = -1;
	ctx->ibv.async_fd = -1; /* no async events yet */
	ctx->ibv.num_comp_vectors = 1;

	rc = PtlEQAlloc(ni->handle, PTL_EQ_SIZE, &ctx->eq);
	if (rc != PTL_OK) {
		PTL_WARN("PtlEQAlloc: %s", PtlToStr(rc, PTL_STR_ERROR));
		goto err_free;
	}

	memset(&md, 0, sizeof(md));
	md.start = 0;
	md.length = PTL_SIZE_MAX;
	md.options = 0;
	md.eq_handle = ctx->eq;
	md.ct_handle = PTL_CT_NONE;
	rc = PtlMDBind(ni->handle, &md, &ctx->md);
	if (rc != PTL_OK) {
		PTL_WARN("PtlMDBind: %s", PtlToStr(rc, PTL_STR_ERROR));
		goto err_eq;
	}

	rc = PtlPTAlloc(ni->handle, 0, ctx->eq, PTL_PT_ANY, &ctx->pte);
	if (rc != PTL_OK) {
		PTL_WARN("PtlPTAlloc: %s", PtlToStr(rc, PTL_STR_ERROR));
		goto err_md;
	}
	rc = PtlPTEnable(ni->handle, ctx->pte);
	if (rc != PTL_OK) {
		PTL_WARN("PtlPTEnable(%u): %s", ctx->pte, PtlToStr(rc, PTL_STR_ERROR));
		goto err_pt;
	}

	memset(&le, 0, sizeof(le));
	le.start = 0;
	le.length = PTL_SIZE_MAX;
	le.ct_handle = PTL_CT_NONE;
	le.uid = PTL_UID_ANY;
	le.match_id.phys.nid = PTL_NID_ANY;
	le.match_id.phys.pid = PTL_PID_ANY;
	/* Persistent (no USE_ONCE). Comm events stay enabled: they carry hdr_data. */
	le.options = PTL_LE_OP_PUT | PTL_LE_OP_GET | PTL_LE_IS_ACCESSIBLE |
		     PTL_LE_EVENT_LINK_DISABLE;
	rc = PtlLEAppend(ni->handle, ctx->pte, &le, PTL_PRIORITY_LIST, ctx, &ctx->le);
	if (rc != PTL_OK) {
		PTL_WARN("PtlLEAppend: %s", PtlToStr(rc, PTL_STR_ERROR));
		goto err_pt;
	}
	PTL_DEBUG("context %p open: nid %u pid %u pte %u", (void *)ctx, ni->id.phys.nid,
		  ni->id.phys.pid, ctx->pte);
	return &ctx->ibv;

err_pt:
	PtlPTFree(ni->handle, ctx->pte);
err_md:
	PtlMDRelease(ctx->md);
err_eq:
	PtlEQFree(ctx->eq);
err_free:
	pthread_mutex_destroy(&ctx->lock);
	pthread_mutex_destroy(&ctx->qp_lock);
	free(ctx->qps);
	free(ctx);
	errno = EIO;
	return NULL;
}

PTL_EXPORT int ptl_close_device(struct ibv_context *context)
{
	struct ptl_ctx *ctx = ptl_ctx_from_ibv(context);
	int rc, retry;

	if (ctx->open_objects != 0)
		PTL_WARN("closing context %p with %d pd/cq/srq still alive", (void *)ctx,
			 ctx->open_objects);

	/* Order from SPDK's ptl_cq_core_release: the LE and PTE report into the EQ and
	 * the MD names it, so the EQ goes last. */
	for (retry = 0, rc = PtlLEUnlink(ctx->le); rc == PTL_IN_USE && retry < 100; retry++) {
		usleep(1000);
		rc = PtlLEUnlink(ctx->le);
	}
	if (rc != PTL_OK)
		PTL_WARN("PtlLEUnlink: %s", PtlToStr(rc, PTL_STR_ERROR));
	rc = PtlPTFree(ctx->ni->handle, ctx->pte);
	if (rc != PTL_OK)
		PTL_WARN("PtlPTFree(%u): %s", ctx->pte, PtlToStr(rc, PTL_STR_ERROR));
	rc = PtlMDRelease(ctx->md);
	if (rc != PTL_OK)
		PTL_WARN("PtlMDRelease: %s", PtlToStr(rc, PTL_STR_ERROR));
	rc = PtlEQFree(ctx->eq);
	if (rc != PTL_OK)
		PTL_WARN("PtlEQFree: %s", PtlToStr(rc, PTL_STR_ERROR));

	ctx->magic = 0;
	pthread_mutex_destroy(&ctx->lock);
	pthread_mutex_destroy(&ctx->qp_lock);
	free(ctx->qps);
	free(ctx);
	return 0;
}

PTL_EXPORT int ptl_query_port(struct ibv_context *context, uint8_t port_num,
			      struct ibv_port_attr *port_attr)
{
	(void)ptl_ctx_from_ibv(context);
	if (port_num != 1)
		return EINVAL;
	memset(port_attr, 0, sizeof(*port_attr));
	port_attr->state = IBV_PORT_ACTIVE;
	port_attr->max_mtu = IBV_MTU_4096;
	port_attr->active_mtu = IBV_MTU_4096;
	port_attr->gid_tbl_len = 1;
	port_attr->max_msg_sz = 1U << 30;
	/* lid 0 makes tensorpipe address the peer by gid, which is where
	 * (nid, pid, pte) travel (utility: transitionIbvQueuePairToReadyToReceive). */
	port_attr->lid = 0;
	port_attr->link_layer = IBV_LINK_LAYER_ETHERNET;
	port_attr->phys_state = 5; /* LinkUp */
	return 0;
}

PTL_EXPORT int ptl_query_gid(struct ibv_context *context, uint8_t port_num, int index,
			     union ibv_gid *gid)
{
	struct ptl_ctx *ctx = ptl_ctx_from_ibv(context);
	struct ptl_gid g;

	if (port_num != 1 || index != 0)
		return EINVAL;
	memcpy(g.magic, "PTL4", 4);
	g.nid = ctx->ni->id.phys.nid;
	g.pid = ctx->ni->id.phys.pid;
	g.pte = ctx->pte;
	memcpy(gid->raw, &g, sizeof(g));
	return 0;
}

PTL_EXPORT struct ibv_pd *ptl_alloc_pd(struct ibv_context *context)
{
	struct ptl_ctx *ctx = ptl_ctx_from_ibv(context);
	struct ptl_pd *pd = calloc(1, sizeof(*pd));

	if (pd == NULL) {
		errno = ENOMEM;
		return NULL;
	}
	pd->magic = PTL_MAGIC_PD;
	pd->ctx = ctx;
	pd->ibv.context = context;
	__atomic_fetch_add(&ctx->open_objects, 1, __ATOMIC_RELAXED);
	return &pd->ibv;
}

PTL_EXPORT int ptl_dealloc_pd(struct ibv_pd *ibv_pd)
{
	struct ptl_pd *pd = ptl_pd_from_ibv(ibv_pd);

	__atomic_fetch_sub(&pd->ctx->open_objects, 1, __ATOMIC_RELAXED);
	pd->magic = 0;
	free(pd);
	return 0;
}
