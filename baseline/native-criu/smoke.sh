#!/usr/bin/env bash
# An actual cross-host dump/restore using unmodified CRIU and the RDMA relay.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
SOURCE_CR=${CRIU_SOURCE:-$ROOT/native-criu/.build/upstream-criu}
CRIU_BIN=${CRIU_BIN:-$SOURCE_CR/criu/criu}
TARGET=${TARGET:-node3}
SOURCE_RDMA_IP=${SOURCE_RDMA_IP:-10.0.0.62}
TARGET_RDMA_IP=${TARGET_RDMA_IP:-10.0.0.63}
TAG=sb-native-$(date +%Y%m%d-%H%M%S)-$$
RUN=/tmp/$TAG
PORT=$((20000 + $$ % 20000))
RELAY_SOURCE_PID= RELAY_TARGET_PID= FIXTURE_PID=

remote() { ssh -oBatchMode=yes "$TARGET" "$@"; }
cleanup() {
    [ -z "$RELAY_SOURCE_PID" ] || kill -TERM "$RELAY_SOURCE_PID" 2>/dev/null || true
    [ -z "$RELAY_TARGET_PID" ] || remote "kill -TERM $RELAY_TARGET_PID 2>/dev/null || true" || true
    [ -z "$FIXTURE_PID" ] || kill -TERM "$FIXTURE_PID" 2>/dev/null || true
}
trap cleanup EXIT

if [ ! -x "$CRIU_BIN" ]; then
    "$ROOT/native-criu/build.sh"
fi
make -C "$ROOT/common" rsocket_relay
mkdir -p "$RUN/images"
remote "mkdir -p '$RUN'; cat > '$RUN/criu'" < "$CRIU_BIN"
remote "cat > '$RUN/rsocket_relay'" < "$ROOT/common/rsocket_relay"
remote "cat > '$RUN/tree_stream.py'" < "$ROOT/common/tree_stream.py"
cc -O2 -Wall -Wextra -Werror -o "$RUN/fixture" "$ROOT/native-criu/memory_fixture.c"
remote "cat > '$RUN/fixture'" < "$RUN/fixture"
remote "chmod +x '$RUN/criu' '$RUN/rsocket_relay' '$RUN/fixture'; mkdir -p '$RUN'"
test "$(sha256sum "$CRIU_BIN" | cut -d' ' -f1)" = \
    "$(remote "sha256sum '$RUN/criu'" | cut -d' ' -f1)"

(cd /tmp; nohup "$RUN/fixture" "$RUN/status" >"$RUN/fixture.log" 2>&1 </dev/null & echo $!) > "$RUN/fixture.pid"
FIXTURE_PID=$(cat "$RUN/fixture.pid")
sleep .4
BEFORE=$(sed -n 's/.*tick=\([0-9]*\).*/\1/p' "$RUN/status")
sudo -n "$CRIU_BIN" dump -t "$FIXTURE_PID" -D "$RUN/images" \
    -o dump.log -v4 --shell-job
if kill -0 "$FIXTURE_PID" 2>/dev/null; then
    echo 'source process survived final dump' >&2
    exit 1
fi

RELAY_TARGET_PID=$(remote "nohup '$RUN/rsocket_relay' listen '$TARGET_RDMA_IP' '$((PORT+1))' 127.0.0.1 '$PORT' >'$RUN/relay-target.log' 2>&1 </dev/null & echo \$!")
remote "nohup python3 '$RUN/tree_stream.py' receive '$RUN/images' --port '$PORT' >'$RUN/transfer-target.log' 2>&1 </dev/null &"
nohup "$ROOT/common/rsocket_relay" connect 127.0.0.1 "$((PORT+2))" "$TARGET_RDMA_IP" "$((PORT+1))" >"$RUN/relay-source.log" 2>&1 </dev/null &
RELAY_SOURCE_PID=$!
sleep .3
sudo -n python3 "$ROOT/common/tree_stream.py" send "$RUN/images" --port "$((PORT+2))" | tee "$RUN/transfer-source.log"
remote "touch '$RUN/fixture.log'; sudo -n '$RUN/criu' restore -D '$RUN/images' -o restore.log -v4 --shell-job --skip-file-rwx-check -d"
sleep .5
AFTER=$(remote "sed -n 's/.*tick=\\([0-9]*\\).*/\\1/p' '$RUN/status'")
PAGES=$(remote "sed -n 's/.*verified_pages=\\([0-9]*\\).*/\\1/p' '$RUN/status'")
test "$PAGES" = 16384
test "$AFTER" -gt "$BEFORE"
sleep .3
LATER=$(remote "sed -n 's/.*tick=\\([0-9]*\\).*/\\1/p' '$RUN/status'")
test "$LATER" -gt "$AFTER"
remote "kill -TERM '$FIXTURE_PID'"
FIXTURE_PID=
echo "PASS native CRIU over RDMA; source_tick=$BEFORE restored_tick=$AFTER later_tick=$LATER run=$RUN"
