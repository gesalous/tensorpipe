/* Prints the limits BXIv2 grants a non-matching physical NI, and how many PTEs one process can really allocate.
 * build: cc -O2 -o ni_limits ni_limits.c -lportals */
#include <portals4.h>
#include <portals4_bxiext.h>
#include <stdio.h>
#include <stdlib.h>
int main(void)
{
	ptl_ni_limits_t a; ptl_handle_ni_t ni; ptl_handle_eq_t eq; int rc, n = 0; ptl_pt_index_t pt;
	if (PtlInit() != PTL_OK || PtlNIInit(PTL_IFACE_DEFAULT, PTL_NI_NO_MATCHING | PTL_NI_PHYSICAL, PTL_PID_ANY, NULL, &a, &ni) != PTL_OK) { puts("init failed"); return 1; }
	printf("max_entries %d max_unexpected %d max_mds %d max_cts %d max_eqs %d max_pt_index %d max_iovecs %d max_list_size %d\n",
	       a.max_entries, a.max_unexpected_headers, a.max_mds, a.max_cts, a.max_eqs, a.max_pt_index, a.max_iovecs, a.max_list_size);
	printf("max_msg_size %lu max_volatile_size %lu\n", (unsigned long)a.max_msg_size, (unsigned long)a.max_volatile_size);
	PtlEQAlloc(ni, 4096, &eq);
	while ((rc = PtlPTAlloc(ni, 0, eq, PTL_PT_ANY, &pt)) == PTL_OK) n++;
	printf("PtlPTAlloc succeeded %d times, then rc=%d (%s)\n", n, rc, PtlToStr(rc, PTL_STR_ERROR));
	return 0;
}
