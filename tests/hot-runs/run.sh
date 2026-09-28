#!/usr/bin/env bash
set -euo pipefail
sb_here=$(cd "$(dirname "$0")" && pwd)
sb_repo=$(cd "$sb_here/../.." && pwd)
sb_tmp=$(mktemp -d /tmp/sbk-hot-runs.XXXXXX)
trap 'rm -rf "$sb_tmp"' EXIT
sb_flags=()
case "${1:-normal}" in
 normal) ;;
 asan) sb_flags=(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie);;
 *) exit 2;;
esac
cc -std=gnu11 -O2 -g -Wall -Wextra -Werror "${sb_flags[@]}" -iquote "$sb_repo/criu-k/criu/include" "$sb_here/test-hot-runs.c" -o "$sb_tmp/test"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 "$sb_tmp/test" "${2:-benchmark}"
