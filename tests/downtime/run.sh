#!/usr/bin/env bash
set -euo pipefail
sb_test=$(cd "$(dirname "$0")" && pwd)
sb_repo=$(cd "$sb_test/../.." && pwd)
sb_out=$(mktemp -d "${TMPDIR:-/tmp}/swiftbaton-downtime-tests.XXXXXX")
trap 'rm -rf "$sb_out"' EXIT
sb_mode=${1:-normal}
sb_flags=(-O2)
case "$sb_mode" in
  normal) ;;
  asan) sb_flags=(-O1 -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie);;
  tsan) sb_flags=(-O1 -fsanitize=thread -fno-omit-frame-pointer -fno-pie -no-pie);;
  *) echo 'usage: bash tests/downtime/run.sh [normal|asan|tsan]' >&2; exit 2;;
esac
python3 "$sb_test/test_migration_window.py"
python3 - "$sb_repo" "$sb_out" <<'PY'
from pathlib import Path
import sys
r,out=map(Path,sys.argv[1:])
def helper(mode):
 s=(r/mode/'criu/cr-dump.c').read_text()
 return s[s.index('struct precopy_final_job {'):s.index('\nint cr_dump_tasks(pid_t pid)')]
s=helper('criu');assert s==helper('criu-k')
(out/'validation-overlap.inc').write_text(s)
PY
cc -g -Wall -Wextra -Werror -pthread "${sb_flags[@]}" \
  -iquote "$sb_out" -iquote "$sb_repo/criu/criu/include" \
  "$sb_test/test_validation_overlap.c" -o "$sb_out/overlap"
"$sb_out/overlap"
cc -g -Wall -Wextra -Werror -pthread "${sb_flags[@]}" \
  -iquote "$sb_repo/criu/criu/include" "$sb_test/test_scheduler_bitmap.c" \
  "$sb_repo/criu/criu/sb-sched.c" -o "$sb_out/scheduler"
"$sb_out/scheduler"
sb_src=$sb_repo/criu-k
cc -g -Wall -Wextra -Werror -pthread "${sb_flags[@]}" \
  -iquote "$sb_src/criu/include" -iquote "$sb_src/include" \
  "$sb_test/sparse-plan.c" "$sb_src/criu/sb-kernel-sparse.c" -o "$sb_out/sparse"
"$sb_out/sparse"
cc -D_GNU_SOURCE -g -Wall -Wextra -Werror -pthread "${sb_flags[@]}" \
  -iquote "$sb_test/ps-stubs" -iquote "$sb_src/criu/include" -iquote "$sb_src/include" \
  "$sb_test/snapshot-stream.c" "$sb_src/criu/sb-kernel-precopy.c" \
  "$sb_src/criu/sb-kernel-sparse.c" "$sb_src/criu/sb-kernel-work.c" \
  -Wl,--wrap=ioctl -Wl,--wrap=process_vm_readv -o "$sb_out/snapshot"
sudo -n env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 TSAN_OPTIONS=halt_on_error=1 "$sb_out/snapshot"
# The fork/mremap stage fixture is covered by ASan/UBSan, not TSan.
if [[ "$sb_mode" != tsan ]]; then
  sb_src=$sb_repo/criu
  cc -g -Wall -Wextra -Werror -pthread "${sb_flags[@]}" \
    -iquote "$sb_test" -iquote "$sb_src/criu/include" -iquote "$sb_src/include" \
    "$sb_src/criu/sb-precopy.c" "$sb_src/criu/sb-stage.c" "$sb_test/test_stage_budget.c" \
    -o "$sb_out/stage"
  sudo -n "$sb_out/stage"
fi
echo "SwiftBaton downtime fixtures PASS ($sb_mode)"
