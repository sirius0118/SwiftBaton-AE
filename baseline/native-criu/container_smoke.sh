#!/usr/bin/env bash
# Full Redis container checkpoint/restore over a cross-host rsocket RDMA link.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
TARGET=${TARGET:-node3}
STOCK=${STOCK_CRIU:-$ROOT/native-criu/.build/upstream-criu/criu/criu}
IMAGE=${SB_REDIS_IMAGE:-m.daocloud.io/docker.io/library/redis:latest}
WORK=${SB_AE_WORK_ROOT:-$(cd "$ROOT/.." && pwd)-work}
mkdir -p "$WORK"
exec 9>"$WORK/run.lock"
flock -n 9 || { echo 'Another AE run holds the migration lock' >&2; exit 1; }
TAG=sb-native-docker-$(date +%Y%m%d-%H%M%S)-$$
RUN=/tmp/$TAG
PORT=$((22000 + $$ % 20000))
NAME=$TAG
RELAY_SOURCE_PID= RELAY_TARGET_PID= CHANGED_SOURCE= CHANGED_TARGET=

remote() { ssh -oBatchMode=yes "$TARGET" "$@"; }
cleanup() {
    [ -z "$RELAY_SOURCE_PID" ] || kill -TERM "$RELAY_SOURCE_PID" 2>/dev/null || true
    [ -z "$RELAY_TARGET_PID" ] || remote "kill -TERM $RELAY_TARGET_PID 2>/dev/null || true" || true
    docker rm -f "$NAME" >/dev/null 2>&1 || true
    remote "docker rm -f '$NAME' >/dev/null 2>&1 || true" || true
    [ -z "$CHANGED_TARGET" ] || remote "sudo -n python3 '$RUN/select_criu.py' '$RUN/criu' '$ORIGINAL_TARGET'" || true
    [ -z "$CHANGED_SOURCE" ] || sudo -n python3 "$ROOT/common/select_criu.py" "$STOCK" "$ORIGINAL_SOURCE" || true
}
trap cleanup EXIT

test -x "$STOCK"
make -C "$ROOT/common" rsocket_relay
test -z "$(pgrep -x criu || true)"
test -z "$(remote 'pgrep -x criu || true')"
test -z "$(docker ps -aq --filter label=swiftbaton.ae=true)"
test -z "$(remote 'docker ps -aq --filter label=swiftbaton.ae=true')"
test "$(docker image inspect "$IMAGE" --format '{{.Id}}')" = \
    "$(remote "docker image inspect '$IMAGE' --format '{{.Id}}'")"
ORIGINAL_SOURCE=$(readlink /usr/bin/criu)
ORIGINAL_TARGET=$(remote 'readlink /usr/bin/criu')
mkdir -p "$RUN"
remote "mkdir -p '$RUN'; cat > '$RUN/criu'" < "$STOCK"
remote "cat > '$RUN/rsocket_relay'" < "$ROOT/common/rsocket_relay"
remote "cat > '$RUN/tree_stream.py'" < "$ROOT/common/tree_stream.py"
remote "cat > '$RUN/select_criu.py'" < "$ROOT/common/select_criu.py"
remote "chmod +x '$RUN/criu' '$RUN/rsocket_relay'"
test "$(sha256sum "$STOCK" | cut -d' ' -f1)" = \
    "$(remote "sha256sum '$RUN/criu'" | cut -d' ' -f1)"

sudo -n python3 "$ROOT/common/select_criu.py" "$ORIGINAL_SOURCE" "$STOCK"
CHANGED_SOURCE=1
remote "sudo -n python3 '$RUN/select_criu.py' '$ORIGINAL_TARGET' '$RUN/criu'"
CHANGED_TARGET=1
docker create --name "$NAME" --label swiftbaton.baseline.probe=true \
    --security-opt seccomp=unconfined "$IMAGE" redis-server --save '' --appendonly no
remote "docker create --name '$NAME' --label swiftbaton.baseline.probe=true --security-opt seccomp=unconfined '$IMAGE' redis-server --save '' --appendonly no"
docker start "$NAME"
docker exec "$NAME" redis-cli SET baseline:sentinel "$TAG"
docker exec "$NAME" redis-cli SET baseline:value "$(printf 'v%.0s' {1..1024})"
SOURCE_ID=$(docker inspect -f '{{.Id}}' "$NAME")
TARGET_ID=$(remote "docker inspect -f '{{.Id}}' '$NAME'")
docker checkpoint create "$NAME" checkpoint
SOURCE_DIR=/var/lib/docker/containers/$SOURCE_ID/checkpoints/checkpoint
TARGET_DIR=/var/lib/docker/containers/$TARGET_ID/checkpoints/checkpoint
remote "sudo -n mkdir -p '/var/lib/docker/containers/$TARGET_ID/checkpoints'"
RELAY_TARGET_PID=$(remote "nohup '$RUN/rsocket_relay' listen 10.0.0.63 '$((PORT+1))' 127.0.0.1 '$PORT' >'$RUN/relay-target.log' 2>&1 </dev/null & echo \$!")
remote "nohup sudo -n python3 '$RUN/tree_stream.py' receive '$TARGET_DIR' --port '$PORT' >'$RUN/transfer-target.log' 2>&1 </dev/null &"
nohup "$ROOT/common/rsocket_relay" connect 127.0.0.1 "$((PORT+2))" 10.0.0.63 "$((PORT+1))" >"$RUN/relay-source.log" 2>&1 </dev/null &
RELAY_SOURCE_PID=$!
sleep .3
sudo -n python3 "$ROOT/common/tree_stream.py" send "$SOURCE_DIR" --port "$((PORT+2))" | tee "$RUN/transfer-source.log"
remote "docker start --checkpoint checkpoint '$NAME'"
SENTINEL=$(remote "docker exec '$NAME' redis-cli GET baseline:sentinel")
VALUE_LENGTH=$(remote "docker exec '$NAME' redis-cli STRLEN baseline:value")
test "$SENTINEL" = "$TAG"
test "$VALUE_LENGTH" = 1024
echo "PASS stock CRIU Redis container over RDMA; sentinel=$SENTINEL value_bytes=$VALUE_LENGTH run=$RUN"
