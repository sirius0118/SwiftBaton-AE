#!/usr/bin/env bash
set -euo pipefail
ae_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
echo 'Stage this checkout and its locally built binaries. Preview is the default.'
exec python3 "$ae_root/scripts/deploy.py" stage "$@"
