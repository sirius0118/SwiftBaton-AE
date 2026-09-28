#!/usr/bin/env bash
set -euo pipefail
sb_here=$(cd "$(dirname "$0")" && pwd)
sb_repo=$(cd "$sb_here/../.." && pwd)
sb_out=$(mktemp -d /tmp/sbk-ps-layout.XXXXXX)
trap 'rm -rf "$sb_out"' EXIT
sb_flags=()
case "${1:-normal}" in
 normal) ;;
 asan) sb_flags=(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie);;
 *) exit 2;;
esac
cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -pthread "${sb_flags[@]}" \
 -iquote "$sb_repo/criu-k/criu/include" -iquote "$sb_repo/criu-k/include" \
 "$sb_here/test-layout.c" "$sb_repo/criu-k/criu/sb-kernel-layout.c" \
 "$sb_repo/criu-k/criu/sb-kernel-sparse.c" -o "$sb_out/test"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 "$sb_out/test"
python3 "$sb_here/test-settings.py"
