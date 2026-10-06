#!/usr/bin/env bash
# Ubuntu 20.04/22.04 build and runner dependencies. Preview unless --execute.
set -euo pipefail
privilege=()
if [[ $(id -u) != 0 ]]; then privilege=(sudo); fi
packages=(build-essential gcc-multilib flex bison bc numactl pkg-config git rsync python3 python3-venv python3-matplotlib python3-numpy
 ca-certificates curl openssh-client sudo procps iproute2 nftables kmod cpio xz-utils
 protobuf-compiler protobuf-c-compiler libprotobuf-dev libprotobuf-c-dev python3-protobuf uthash-dev
 libnl-3-dev libnl-route-3-dev libcap-dev libnet1-dev libaio-dev
 libgnutls28-dev libnftables-dev libibverbs-dev librdmacm-dev
 libelf-dev libbpf-dev libbsd-dev libselinux1-dev libseccomp-dev libssl-dev zlib1g-dev
 libnetfilter-queue1 libnetfilter-conntrack3
 openjdk-8-jdk maven redis-tools iptables conntrack rdma-core ibverbs-utils)
printf '%q ' "${privilege[@]}" apt-get -o APT::Update::Error-Mode=any -o Acquire::Retries=3 update; printf '\n'
printf '%q ' "${privilege[@]}" apt-get install -y --no-install-recommends "${packages[@]}"; printf '\n'
case "${1:-}" in
  --execute)
    "${privilege[@]}" apt-get -o APT::Update::Error-Mode=any -o Acquire::Retries=3 update
    "${privilege[@]}" apt-get install -y --no-install-recommends "${packages[@]}" ;;
  '') echo 'PREVIEW ONLY. Add --execute to install these packages.' ;;
  *) echo 'Usage: bash scripts/install-build-deps.sh [--execute]' >&2; exit 2 ;;
esac
