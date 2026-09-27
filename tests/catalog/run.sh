#!/usr/bin/env bash
set -euo pipefail
sb_test=$(cd "$(dirname "$0")" && pwd)
sb_repo=$(cd "$sb_test/../.." && pwd)
sb_out=$(mktemp -d "${TMPDIR:-/tmp}/swiftbaton-catalog-tests.XXXXXX")
trap 'rm -rf "$sb_out"' EXIT
sb_mode=${1:-normal}
sb_flags=()
case "$sb_mode" in
 normal) ;;
 asan) sb_flags=(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie);;
 tsan) sb_flags=(-fsanitize=thread -fno-omit-frame-pointer -fno-pie -no-pie);;
 *) exit 2;;
esac
for sb_name in catalog-stage test-final-prepare; do
 sb_wrap=();if [[ $sb_name == test-final-prepare ]];then sb_wrap=(-Wl,--wrap=pthread_create);fi
 cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -pthread "${sb_flags[@]}" \
  -iquote "$sb_repo/criu-k/criu/include" -iquote "$sb_repo/criu-k/include" \
  "$sb_test/$sb_name.c" "$sb_repo/criu-k/criu/sb-kernel-catalog.c" \
  -Wl,--wrap=ioctl -Wl,--wrap=open "${sb_wrap[@]}" -o "$sb_out/$sb_name"
 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 TSAN_OPTIONS=halt_on_error=1 "$sb_out/$sb_name"
done
echo "Catalog ownership fixtures PASS ($sb_mode); device ioctls are mocked."
