#!/usr/bin/env bash
set -euo pipefail
sb_here=$(cd "$(dirname "$0")" && pwd)
sb_repo=$(cd "$sb_here/../.." && pwd)
sb_tmp=$(mktemp -d /tmp/sbk-dma-wire.XXXXXX)
trap 'rm -rf "$sb_tmp"' EXIT
sb_flags=()
case "${1:-normal}" in
 normal) ;;
 asan) sb_flags=(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie);;
 *) exit 2;;
esac
cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -pthread "${sb_flags[@]}" \
 -iquote "$sb_repo/criu-k/criu/include" -iquote "$sb_repo/criu-k/include" \
 "$sb_here/test-wire.c" -Wl,--wrap=ioctl -o "$sb_tmp/test"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 "$sb_tmp/test"
python3 "$sb_here/test-settings.py"
