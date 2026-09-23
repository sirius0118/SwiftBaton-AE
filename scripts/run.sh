#!/usr/bin/env bash
# Always previews unless --execute is supplied. Must be invoked on knode2.
set -euo pipefail
ae_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
profile=${1:-smoke}
if (($#)); then shift; fi
common=(--image-rdma --fast-cutover --u-precopy --parent-stage --parallel-transfer
        --validation-workers 8 --vma-cache --fault-workers 1 --prefetch-workers 1
        --copy-workers 4 --install-workers 4 --batch-pages 64 --prefetch-window 4)
case "$profile" in
  smoke) parameters=(--records 100000 --duration 45 --warmup 10 --threads 16 --canary-mib 8) ;;
  redis) parameters=(--records 1000000 --duration 90 --warmup 15 --threads 32 --canary-mib 8) ;;
  stress) parameters=(--records 1000000 --duration 90 --warmup 15 --threads 32 --canary-mib 8
                      --memory-children 3 --child-mib 64 --child-workers 4 --cow-descendants --dynamic-memory) ;;
  *) echo 'Usage: bash scripts/run.sh {smoke|redis|stress} [--execute] [driver options]' >&2; exit 2 ;;
esac
exec python3 -u "$ae_root/DualDriver/script/run_ae.py" "${common[@]}" "${parameters[@]}" "$@"
