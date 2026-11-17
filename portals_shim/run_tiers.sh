#!/usr/bin/env bash
# Runs the Ibv unit tests against the Portals shim, simplest tier first, and
# stops at the first failing tier (so you implement in ladder order).
# See design.md section 9 for what each tier needs.
#
#   BUILD_DIR=...  build tree holding tensorpipe/test/tensorpipe_test and
#                  portals_shim/libtensorpipe_portals.so (default: ../build-borg1)
#   KEEP_GOING=1   run every tier even after a failure
#   STOCK=1        run against stock libibverbs instead (baseline/control)
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$HERE/../build-borg1}"
TEST="$BUILD_DIR/tensorpipe/test/tensorpipe_test"
SHIM="$BUILD_DIR/portals_shim/libtensorpipe_portals.so"
T=Ibv/TransportTest
I=Ibv/IbvTransportTest

tiers=(
  "T0 no verbs|IbvSockaddr.*"
  "T1 device/context bring-up|$T.Context_DomainDescriptor/0:$T.Listener_CreateAfterClosingContextAndThenGetAddress/0"
  "T2 QP + MR setup, no data|$T.Context_Basics/0:$T.Listener_Basics/0:$T.Listener_CreateThenCloseAndThenGetAddress/0:$T.Listener_AcceptCallbacksAreQueued/0:$T.Listener_IncomingConnectionsAreQueued/0:$T.Connection_InitializationError/0"
  "T3 first data on the wire|$T.Connection_Initialization/0:$T.Connection_NopWrite/0"
  "T4 many writes (ordering)|$T.Connection_QueueWritesBeforeReads/0:$I.QueueWrites/0:$I.NopWriteWrapAround/0"
  "T5 large / chunked|$I.Chunking/0:$I.ChunkingImplicitRead/0"
)

[ "${STOCK:-0}" = 1 ] && unset TP_IBV_LIBRARY || export TP_IBV_LIBRARY="$SHIM"
rc=0
for tier in "${tiers[@]}"; do
  name="${tier%%|*}"; filter="${tier#*|}"
  if ( timeout 300 "$TEST" --gtest_filter="$filter" > /tmp/tier.$$.log 2>&1 ) 2>/dev/null; then
    echo "PASS  $name"
  else
    echo "FAIL  $name"; grep -E "FATAL|FAILED|Failure" /tmp/tier.$$.log | head -5 | sed 's/^/        /'
    rc=1; [ "${KEEP_GOING:-0}" = 1 ] || break
  fi
done
rm -f /tmp/tier.$$.log
exit $rc
