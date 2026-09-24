#!/usr/bin/env bash
# Ubuntu 20.04 build dependencies. Print commands unless --execute is given.
set -euo pipefail
packages=(build-essential gcc-multilib flex bison bc numactl pkg-config git rsync python3 python3-venv python3-matplotlib python3-numpy
 protobuf-compiler protobuf-c-compiler libprotobuf-dev libprotobuf-c-dev
 libnl-3-dev libnl-route-3-dev libcap-dev libnet1-dev libaio-dev
 libgnutls28-dev libnftables-dev libibverbs-dev librdmacm-dev
 libelf-dev libbpf-dev libbsd-dev libselinux1-dev libseccomp-dev zlib1g-dev
 openjdk-8-jdk maven redis-tools iptables conntrack rdma-core ibverbs-utils)
printf '%q ' sudo apt-get update; printf '\n'
printf '%q ' sudo apt-get install -y "${packages[@]}"; printf '\n'
case "${1:-}" in
  --execute) sudo apt-get update; sudo apt-get install -y "${packages[@]}" ;;
  '') echo 'PREVIEW ONLY. Add --execute to install these packages.' ;;
  *) echo 'Usage: bash scripts/install-build-deps.sh [--execute]' >&2; exit 2 ;;
esac
