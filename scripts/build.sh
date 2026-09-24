#!/usr/bin/env bash
# Compile source copies; never replace installed runtime programs or load modules.
set -euo pipefail
sb_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
sb_jobs=${SB_BUILD_JOBS:-12}
case "${1:-help}" in
  U|K)
    sb_mode=$1; sb_src=criu
    if [[ $sb_mode == K ]]; then sb_src=criu-k; fi
    mkdir -p "$sb_root/build/criu-$sb_mode"
    rsync -a --exclude=.git/ "$sb_root/$sb_src/" "$sb_root/build/criu-$sb_mode/"
    make -C "$sb_root/build/criu-$sb_mode" -j"$sb_jobs" criu
    sha256sum "$sb_root/build/criu-$sb_mode/criu/criu" ;;
  ycsb)
    mkdir -p "$sb_root/build/YCSB"
    rsync -a --exclude=.git/ --exclude=target/ "$sb_root/YCSB/" "$sb_root/build/YCSB/"
    cd "$sb_root/build/YCSB"
    mvn -pl redis -am package -DskipTests -Dcheckstyle.skip=true
    mvn -pl core dependency:copy-dependencies -DincludeScope=runtime
    test -f core/target/classes/site/ycsb/Client.class
    test -f redis/target/classes/site/ycsb/db/RedisClient.class ;;
  fixture)
    mkdir -p "$sb_root/build/fixture"
    gcc -O2 -static -pthread "$sb_root/scripts/ae/k/memory_fixture.c" -o "$sb_root/build/fixture/memory_fixture" ;;
  module)
    : "${SB_KERNEL_BUILD:?Set SB_KERNEL_BUILD to the matching configured kernel build tree}"
    : "${SB_OFED_BUILD:?Set SB_OFED_BUILD to the matching OFED headers and Module.symvers directory}"
    sb_release=$(make -s -C "$SB_KERNEL_BUILD" kernelrelease)
    sb_module="$sb_root/build/module-$sb_release"
    mkdir -p "$sb_module"
    rsync -a "$sb_root/kernel/module/" "$sb_module/"
    rsync -a "$sb_root/kernel/include/" "$sb_root/build/include/"
    make -C "$sb_module" -j"$sb_jobs" KDIR="$SB_KERNEL_BUILD" OFA_DIR="$SB_OFED_BUILD"
    modinfo "$sb_module/swiftbaton_k.ko" | head -12 ;;
  *) echo 'Usage: bash scripts/build.sh {U|K|ycsb|fixture|module}'; exit 2 ;;
esac
