#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BASE="$ROOT/build/criu-U"
OUT="$ROOT/build/criu-U-pclive"
if [ ! -d "$BASE" ]; then
    echo "Build the sealed U binary in $BASE first" >&2
    exit 1
fi
if [ ! -d "$OUT" ]; then
    cp -a "$BASE" "$OUT"
fi
rsync -a "$ROOT/criu/" "$OUT/"
make -C "$OUT" -j 16 criu
sha256sum "$OUT/criu/criu"

if [ "${1:-}" = --stage-target ]; then
    remote="$OUT/criu/criu"
    ssh -oBatchMode=yes knode3 "mkdir -p '$OUT/criu'"
    rsync -a "$remote" "knode3:$remote"
    ssh -oBatchMode=yes knode3 "sha256sum '$remote'"
elif [ "$#" -gt 0 ]; then
    echo "usage: $0 [--stage-target]" >&2
    exit 2
fi
