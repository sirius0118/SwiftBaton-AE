#!/usr/bin/env bash
set -euo pipefail

# Unmodified upstream CRIU v3.18. Never resolve /usr/bin/criu on these hosts:
# that path points at the locally modified SwiftBaton binary.
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
COMMIT=4c1a2ac41bb80843c927d2fde8f2ff4186f8d278
SOURCE=${CRIU_SOURCE:-$ROOT/.build/upstream-criu}
if [ ! -d "$SOURCE/.git" ]; then
    git clone https://github.com/checkpoint-restore/criu.git "$SOURCE"
fi
git -C "$SOURCE" fetch --depth=1 origin "$COMMIT"
git -C "$SOURCE" checkout --detach "$COMMIT"
test "$(git -C "$SOURCE" rev-parse HEAD)" = "$COMMIT"
make -C "$SOURCE" -j "$(nproc)" criu
"$SOURCE/criu/criu" --version
sha256sum "$SOURCE/criu/criu"
