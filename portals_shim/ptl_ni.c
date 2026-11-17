/*
 * Process-wide Portals network interface. One per process, created on first use
 * and released at exit: tensorpipe opens and closes contexts repeatedly and
 * re-initialising the NI each time buys nothing. Non-matching, physical
 * addressing, as in the SPDK bxiv2-nonmatching port.
 */
#include "ptl_internal.h"
#include <errno.h>
#include <stdlib.h>

static struct {
	pthread_mutex_t lock;
	bool tried;
	bool ok;
	struct ptl_ni ni;
} g_ni = {.lock = PTHREAD_MUTEX_INITIALIZER};

static void ptl_ni_fini(void)
{
	if (!g_ni.ok)
		return;
	g_ni.ok = false;
	PtlNIFini(g_ni.ni.handle);
	PtlFini();
}

const struct ptl_ni *ptl_ni_get(void)
{
	ptl_ni_limits_t actual;
	int rc;

	pthread_mutex_lock(&g_ni.lock);
	if (!g_ni.tried) {
		g_ni.tried = true;
		rc = PtlInit();
		if (rc != PTL_OK) {
			PTL_WARN("PtlInit failed: %s", PtlToStr(rc, PTL_STR_ERROR));
			goto out;
		}
		/* NULL desired limits: the library defaults (2.3.7 grants
		 * min(desired, hardware), so a zeroed struct would ask for nothing). */
		rc = PtlNIInit(PTL_IFACE_DEFAULT, PTL_NI_NO_MATCHING | PTL_NI_PHYSICAL,
			       PTL_PID_ANY, NULL, &actual, &g_ni.ni.handle);
		if (rc != PTL_OK) {
			PTL_WARN("PtlNIInit failed: %s", PtlToStr(rc, PTL_STR_ERROR));
			PtlFini();
			goto out;
		}
		rc = PtlGetPhysId(g_ni.ni.handle, &g_ni.ni.id);
		if (rc != PTL_OK) {
			PTL_WARN("PtlGetPhysId failed: %s", PtlToStr(rc, PTL_STR_ERROR));
			PtlNIFini(g_ni.ni.handle);
			PtlFini();
			goto out;
		}
		g_ni.ok = true;
		atexit(ptl_ni_fini);
		PTL_DEBUG("NI up: nid %u pid %u", g_ni.ni.id.phys.nid, g_ni.ni.id.phys.pid);
	}
out:
	pthread_mutex_unlock(&g_ni.lock);
	if (!g_ni.ok) {
		errno = ENODEV;
		return NULL;
	}
	return &g_ni.ni;
}
