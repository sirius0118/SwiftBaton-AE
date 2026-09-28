#!/bin/sh
set -eu
D=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec python3 "$D/../../run_case.py" --case "$D" "$@"
