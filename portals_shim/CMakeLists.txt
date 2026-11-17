# Sprint 0: a verbs-compatible shim exporting ptl_* (see ptl_verbs.c).
# Build with -DTP_BUILD_PORTALS_SHIM=ON. Use at run time with
#   TP_IBV_LIBRARY=<build>/portals_shim/libtensorpipe_portals.so
cmake_minimum_required(VERSION 3.5)
project(tensorpipe_portals_shim C)

find_library(IBVERBS_LIB ibverbs REQUIRED)

add_library(tensorpipe_portals SHARED ptl_verbs.c)
set_target_properties(tensorpipe_portals PROPERTIES
  C_VISIBILITY_PRESET hidden
  C_STANDARD 11)
target_compile_options(tensorpipe_portals PRIVATE -Wall -Wextra)
# Sprint 0 only: forwards to the real libibverbs. Dropped once Portals-backed.
target_link_libraries(tensorpipe_portals PRIVATE ${IBVERBS_LIB})
