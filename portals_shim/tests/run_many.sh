#!/bin/bash
# Runs tests/xhost_many across two hosts.  usage: run_many.sh <server-host> <client-host> <connections> [env...]
BUILD=${BUILD_DIR:-$(cd "$(dirname "$0")/../.." && pwd)/build-borg1}
LOGD=${LOGD:-$BUILD/../../logs}; mkdir -p "$LOGD"
S=$1; C=$2; N=$3; shift 3; ENVV="$*"
SSH="ssh -o BatchMode=yes -J shark2"
rm -f "$LOGD/xm.addr" "$LOGD/xm-server.log" "$LOGD/xm-client.log"
IP=$($SSH "$S" "hostname -I" 2>/dev/null | awk '{print $1}')
$SSH "$S" "cd $BUILD && ulimit -n 65536; $ENVV TP_IBV_LIBRARY=\$PWD/portals_shim/libtensorpipe_portals.so timeout 280 portals_shim/xhost_many server $LOGD/xm.addr $IP $N > $LOGD/xm-server.log 2>&1; echo exit=\$? >> $LOGD/xm-server.log" >/dev/null 2>&1 &
sleep 3
$SSH "$C" "cd $BUILD && ulimit -n 65536; $ENVV TP_IBV_LIBRARY=\$PWD/portals_shim/libtensorpipe_portals.so timeout 270 portals_shim/xhost_many client $LOGD/xm.addr $N > $LOGD/xm-client.log 2>&1; echo exit=\$? >> $LOGD/xm-client.log" >/dev/null 2>&1
wait; sleep 3
rc=0
for f in client server; do
  r=$(grep -E '^(client|server): ' "$LOGD/xm-$f.log" | tail -1); x=$(grep '^exit=' "$LOGD/xm-$f.log")
  echo "$f: ${r:-NO RESULT} $x"
  [[ "$r" == *PASSED* && "$x" == exit=0 ]] || { rc=1; grep -v '^\(client\|server\):' "$LOGD/xm-$f.log" | sort | uniq -c | sort -rn | head -5; }
done
exit $rc
