# Verbs-compatible shim exporting ptl_* (see ptl_verbs.c, design.md).
# Build with -DTP_BUILD_PORTALS_SHIM=ON. Use at run time with
#   TP_IBV_LIBRARY=<build>/portals_shim/libtensorpipe_portals.so
cmake_minimum_required(VERSION 3.5)
project(tensorpipe_portals_shim C)

find_library(PORTALS_LIB portals REQUIRED)

add_library(tensorpipe_portals SHARED ptl_verbs.c ptl_device.c ptl_cq.c ptl_srq.c ptl_ni.c ptl_log.c ptl_mr.c ptl_qp.c ptl_data.c)
set_target_properties(tensorpipe_portals PROPERTIES
  C_VISIBILITY_PRESET hidden
  C_STANDARD 11)
# Stubs ignore their arguments: silence that until they are implemented.
target_compile_options(tensorpipe_portals PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(tensorpipe_portals PRIVATE ${PORTALS_LIB} pthread)
