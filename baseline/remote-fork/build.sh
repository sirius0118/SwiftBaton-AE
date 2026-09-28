#!/usr/bin/env bash
# Build the remote-fork K profile from this checkout without changing installed CRIU.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
out="$root/build/criu-K-rsocket"
mkdir -p "$out"
rsync -a --exclude=.git/ "$root/criu-k/" "$out/"
make -C "$out" -j"${SB_BUILD_JOBS:-12}" criu
sha256sum "$out/criu/criu"
case "${1:-}" in
  '') ;;
  --stage-target)
    ssh -oBatchMode=yes knode3 "mkdir -p '$out/criu'"
    rsync -a "$out/criu/criu" "knode3:$out/criu/criu"
    ssh -oBatchMode=yes knode3 "sha256sum '$out/criu/criu'"
    ;;
  *) echo "usage: $0 [--stage-target]" >&2; exit 2 ;;
esac
