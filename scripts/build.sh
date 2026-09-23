#!/usr/bin/env bash
# Compile source locally. Never install host software or start an experiment.
set -euo pipefail
ae_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
mode=${1:-all}
if [[ $(uname -s) != Linux ]]; then echo 'Build on an x86-64 Linux experiment host.' >&2; exit 2; fi
case "$mode" in
  all)
    bash "$0" criu
    bash "$0" ycsb
    bash "$0" fixture
    python3 "$ae_root/scripts/preflight.py" --offline --built ;;
  criu)
    make -C "$ae_root/criu" -j"${AE_BUILD_JOBS:-2}" criu
    sha256sum "$ae_root/criu/criu/criu" ;;
  ycsb)
    cd "$ae_root/YCSB"
    mvn -B -pl redis -am package -DskipTests -Dcheckstyle.skip=true -Dassembly.skipAssembly=true
    test -f redis/target/classes/site/ycsb/db/RedisClient.class
    test -f core/target/classes/site/ycsb/Client.class
    compgen -G 'redis/target/dependency/jedis-*.jar' >/dev/null ;;
  fixture)
    gcc -O2 -static -pthread "$ae_root/DualDriver/script/memory_fixture.c" -o "$ae_root/DualDriver/script/memory_fixture" ;;
  module)
    make -C "/lib/modules/$(uname -r)/build" M="$ae_root/criu/module" modules ;;
  *) echo 'Usage: bash scripts/build.sh {all|criu|ycsb|fixture|module}' >&2; exit 2 ;;
esac
