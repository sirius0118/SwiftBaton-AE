#!/usr/bin/env bash
# Export the engine's pinned runtime bundle; do not install/restart services.
set -euo pipefail
sb_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
sb_out=${SB_RUNTIME_OUT:-"$sb_root/build/runtime"}
python3 "$sb_root/scripts/restore-source-metadata.py" --repair
mkdir -p "$sb_out"
sb_out=$(cd -- "$sb_out" && pwd)
sb_engine="$sb_root/dependencies/docker-ce/components/engine"
test -f "$sb_engine/hack/make/binary-daemon"
# The binary target skips the unrelated frozen test-image/CRIU stages in final.
DOCKER_BUILDKIT=1 docker build --pull --no-cache --target binary \
  --build-arg "DOCKER_GITCOMMIT=${SB_SOURCE_REVISION:-source-archive}" \
  --output "type=local,dest=$sb_out" -f "$sb_engine/Dockerfile" "$sb_engine"
for sb_binary in dockerd docker-proxy runc containerd containerd-shim containerd-shim-runc-v2 ctr docker-init; do
  test -x "$sb_out/binary-daemon/$sb_binary" || {
    echo "Missing runtime build output: $sb_out/binary-daemon/$sb_binary" >&2
    exit 1
  }
done
echo "Runtime bundle: $sb_out/binary-daemon"
