# SwiftBaton-K host kernel and module

The prepared destination already boots `5.15.167-swiftbaton-k1`. A reviewer using that cluster starts at the main README's CRIU/YCSB build steps. This document describes the source changes and administrator steps for reproducing the kernel setup. None of the AE scripts installs a kernel or reboots a host.

## What changed

`kernel/patches/linux-5.15.167-sbk-pte.patch` applies to Linux **5.15.167**. It adds `CONFIG_SWIFTBATON_PTE`, `include/linux/swiftbaton_pte.h`, and `mm/swiftbaton_pte.c`; integrates the marker with swap-entry classification, faults, fork/zap, mincore and khugepaged; and exports the provider interface to the module. The marker uses a dedicated non-present swap-entry type with token IDs, rather than a bit that already has present-PTE semantics.

The module is in `kernel/module/`; `kernel/include/sbk_uapi.h` defines the CRIU/module ABI. Its source side exposes frozen application pages through registered RDMA memory regions. Its destination side owns PS cache pages, independent demand/FT/BG QP/CQ pools, the session scheduler, and final anonymous-page installation. The MM bridge preserves page permissions, anonymous rmap, memcg/LRU and COW behavior. Marker references track fork and moved aliases; the source cannot retire merely because every unique page was fetched.

The **destination needs the patched kernel**. The source can use stock 5.15.167 because it does not install remote PTE markers, but its module still must be compiled for that source kernel and its RDMA driver ABI. Source and target `.ko` files are not interchangeable.

## Build the kernel in isolation

Obtain the upstream Linux 5.15.167 source from [kernel.org](https://cdn.kernel.org/pub/linux/kernel/v5.x/linux-5.15.167.tar.xz), verifying the upstream signature according to the kernel release instructions. Keep a pristine extracted tree outside this checkout, then run:

```bash
python3 kernel/build.py /path/to/linux-5.15.167 --jobs 8
```

The helper first checks the version and performs a patch dry run. It creates `build/linux-5.15.167-swiftbaton-k1/`, applies the patch and `kernel/config-5.15.167-swiftbaton-k1`, then builds `bzImage` and modules. It neither alters the input source nor writes `/boot` or `/lib/modules`.

The configuration is the tested x86-64 host configuration, with `CONFIG_SWIFTBATON_PTE=y`, local version `-swiftbaton-k1`, and performance builds without KASAN/lockdep. Confirm storage, network and console drivers fit any different hardware. Retain the prior bootable kernel and console access when provisioning another machine.

## Match the RDMA driver ABI

The destination uses an OFED driver build matched to this exact patched kernel. A successful module compile against unrelated distro/inbox headers is insufficient: symbol CRCs and the loaded RDMA ABI must match.

The prepared candidate used this source package (extracted for building, not installed as an unreviewed DKMS replacement):

```text
https://linux.mellanox.com/public/repo/mlnx_ofed/5.8-6.0.4.2/ubuntu20.04/x86_64/mlnx-ofed-kernel-dkms_5.8-OFED.5.8.6.0.4.1_all.deb
SHA256 716f23f1e94526019d8b14f90d8d9c945a5ebbb3eda1810b85630bf0327a8dd2
```

Extract it with `dpkg-deb -x`, copy `usr/src/mlnx-ofed-kernel-5.8` into `build/ofed-target`, and build against the kernel tree above. The configuration used for the matching candidate is:

```bash
SB_KERNEL_BUILD=$PWD/build/linux-5.15.167-swiftbaton-k1
cd build/ofed-target
./configure --kernel-version=5.15.167-swiftbaton-k1 \
  --kernel-sources="$SB_KERNEL_BUILD" --with-linux="$SB_KERNEL_BUILD" \
  --with-linux-obj="$SB_KERNEL_BUILD" \
  --with-core-mod --with-user_access-mod --with-user_mad-mod \
  --with-addr_trans-mod --with-mlx5-mod --with-mlxfw-mod \
  --with-ipoib-mod --with-srp-mod --with-rxe-mod \
  --with-iser-mod --with-isert-mod --with-nfsrdma-mod \
  --with-nvmf_host-mod --with-nvmf_target-mod \
  --without-ipoib_debug-mod --without-mlx5_debug-mod --without-debug-info \
  --with-njobs=8
make -j8 kernel
cd ../..
```

For an independently managed host, the administrator must install the kernel, its modules, and these matching OFED modules into that kernel's module directory, run `depmod`, generate its initramfs, and add/verify its boot entry. Use the host's existing kernel/OFED installation procedure; do not restart an active RDMA stack to apply these changes in place. Boot the new destination kernel in a reserved maintenance window and verify storage, SSH, `mlx5_1` and GID3 before loading SwiftBaton. The artifact deliberately does not automate boot selection or overwrite the prior kernel.

## Build the SwiftBaton module

From the AE repository, build the target module:

```bash
SB_KERNEL_BUILD=$PWD/build/linux-5.15.167-swiftbaton-k1 \
SB_OFED_BUILD=$PWD/build/ofed-target \
  bash scripts/build.sh module
```

For the source module, use the **source's actual** configured kernel build tree and corresponding OFED headers/`Module.symvers`:

```bash
SB_KERNEL_BUILD=/path/to/source-kernel-build \
SB_OFED_BUILD=/path/to/source-ofed-build \
  bash scripts/build.sh module
```

The source OFED tree must match the loaded drivers; do not assume `/usr/src/ofa_kernel/default` is current. In the prepared setup that symlink can point to older 5.4 headers, while the running source RDMA stack uses matching 5.8 headers.

The module outputs are `build/module-<kernelrelease>/swiftbaton_k.ko`. The script copies module sources to that ignored build directory. It does not call `insmod` or `rmmod`.

Before installation, compare `modinfo -F vermagic` with `uname -r` on the intended host and verify the imports listed by `modprobe --dump-modversions <module.ko>` against the kernel and OFED `Module.symvers`. The initial token/MM implementation was exercised with KASAN/RXE fixtures before host deployment; an independently modified patch needs its own kernel safety validation.

## Load on an idle host

On each host, with its matching module file:

```bash
sudo insmod /path/to/matching/swiftbaton_k.ko session_dispatch=1
cat /sys/module/swiftbaton_k/parameters/session_dispatch
cat /sys/module/swiftbaton_k/refcnt
ls -l /dev/swiftbaton_k
```

The expected dispatcher value is `Y`; reference count is `0` before migration. Do not unload a module with live sessions or unresolved destination markers. Use the main README's `run.py K --check` and the driver's K preflight for feature/transport readiness. Kernel/module builds are local outputs and must not be committed to the source repository.
