#!/bin/bash
# Runs tests/xhost_echo across two hosts through the shim.
#   usage: run_xhost.sh <server-host> <client-host> [extra env, e.g. TP_PTL_TEST_SWAP=1 TP_PTL_LOG=2]
# Hosts are reached as "ssh -J shark2 <host>"; the build dir and log dir are on the shared NFS.
BUILD=${BUILD_DIR:-$(cd "$(dirname "$0")/../.." && pwd)/build-borg1}
LOGD=${LOGD:-$BUILD/../../logs}; mkdir -p "$LOGD"
S=$1; C=$2; shift 2; ENVV="$*"
SSH="ssh -o BatchMode=yes -J shark2"
rm -f "$LOGD/xh.addr"
IP=$($SSH "$S" "hostname -I" 2>/dev/null | awk '{print $1}')
$SSH "$S" "cd $BUILD && $ENVV TP_IBV_LIBRARY=\$PWD/portals_shim/libtensorpipe_portals.so timeout 120 portals_shim/xhost_echo server $LOGD/xh.addr $IP > $LOGD/xh-server.log 2>&1; echo exit=\$? >> $LOGD/xh-server.log" >/dev/null 2>&1 &
sleep 3
$SSH "$C" "cd $BUILD && $ENVV TP_IBV_LIBRARY=\$PWD/portals_shim/libtensorpipe_portals.so timeout 100 portals_shim/xhost_echo client $LOGD/xh.addr > $LOGD/xh-client.log 2>&1; echo exit=\$? >> $LOGD/xh-client.log" >/dev/null 2>&1
wait; sleep 3
rc=0
for f in client server; do
  r=$(grep -E '^(client|server): (PASSED|FAILED)' "$LOGD/xh-$f.log" | tail -1); x=$(grep '^exit=' "$LOGD/xh-$f.log")
  echo "$f ($([ $f = server ] && echo $S || echo $C)): ${r:-NO RESULT} $x early=$(grep -c 'arrived early' "$LOGD/xh-$f.log") warn=$(grep -c WARN "$LOGD/xh-$f.log")"
  [[ "$r" == *PASSED* && "$x" == exit=0 ]] || rc=1
done
grep -E 'burst' "$LOGD/xh-client.log"
exit $rc
