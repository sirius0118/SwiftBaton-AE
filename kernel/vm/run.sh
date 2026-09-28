#!/usr/bin/env bash
set -euo pipefail
sbk_root=$(cd "$(dirname "$0")/.." && pwd)
kernel_root=${KDIR:-/home/k8s/exper/zxz/linux-5.15.167}
mkdir -p "$sbk_root/vm/root"/{bin,proc,sys,dev,tmp}
cp /bin/busybox "$sbk_root/vm/root/bin/busybox"
if [[ ${SBK_VM_OFED:-0} == 1 ]]; then
    export SBK_OFED_ROOT="$sbk_root/build/ofed58/lib/modules"
    cp "${SBK_MODULE_PATH:-$sbk_root/build/module-ofed/swiftbaton_k.ko}" "$sbk_root/vm/root/swiftbaton_k.ko"
    cp "$sbk_root/vm/init-ofed" "$sbk_root/vm/root/init"
else
    unset SBK_OFED_ROOT
    cp "${SBK_MODULE_PATH:-$sbk_root/module/swiftbaton_k.ko}" "$sbk_root/vm/root/swiftbaton_k.ko"
    cp "$sbk_root/vm/init-test" "$sbk_root/vm/root/init"
fi
cp "$sbk_root/tests/sbk_test" "$sbk_root/vm/root/"
sha256sum "$sbk_root/vm/root/swiftbaton_k.ko" "$sbk_root/vm/root/sbk_test"
if [[ ${SBK_COMPEL_TEST:-0} == 1 ]]; then
    cp "$sbk_root/tests/criu/sbk_compel_test" "$sbk_root/vm/root/"
fi
chmod +x "$sbk_root/vm/root/init"
python3 "$sbk_root/vm/prepare.py"
(cd "$sbk_root/vm/root" && find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip -1 > ../test-initrd.gz)
stamp=$(date +%Y%m%d_%H%M%S)
log="$sbk_root/vm/test-${SBK_VM_OFED:-0}-$stamp.log"
sudo -n timeout 180 qemu-system-x86_64 -enable-kvm -cpu host -m "${SBK_VM_RAM:-1024}" -smp 4 \
    -nodefaults -nographic -monitor none -serial stdio -no-reboot \
    -kernel "$kernel_root/arch/x86/boot/bzImage" -initrd "$sbk_root/vm/test-initrd.gz" \
    -append "console=ttyS0 panic=-1 init=/init nokaslr sbk_anon=${SBK_TEST_ANON:-0} sbk_compel=${SBK_COMPEL_TEST:-0} sbk_drop_memlock=${SBK_DROP_MEMLOCK:-0}" > "$log" 2>&1
tail -n 35 "$log"
python3 - "$log" "${SBK_VM_OFED:-0}" "${SBK_COMPEL_TEST:-0}" "${SBK_TEST_ANON:-0}" <<'PY'
import pathlib, sys, re
raw = pathlib.Path(sys.argv[1]).read_text()
# Serial printk may split one userspace marker across two console lines.
# Only reconstruct marker text; all kernel-failure checks use untouched raw.
s = re.sub(r'\[\s*\d+\.\d+\] [^\n]*\n', '', raw)
assert 'SBK_VM_TESTS_PASS' in s and 'SBK_TEST_EXIT=0' in s and 'SBK_VM_DONE' in s
expected = 'anonymous_PTE' if sys.argv[4] == '1' else 'file_fixture'
assert 'SBK_MAPPING_MODE=' + expected in s
if sys.argv[2] == '0':
    assert 'SBK_VM_RDMA_TESTS_PASS' in s
if sys.argv[3] == '1':
    assert 'SBK_CRIU_COMPEL_PASS' in s and 'SBK_COMPEL_EXIT=0' in s
assert 'SBK_TAINT=12288' in s, 'Unexpected taint (4096 out-of-tree + 8192 unsigned)'
test_output = raw.split('SBK_VM_BOOT_OK', 1)[1]
assert not any(x in test_output for x in ('BUG:', 'WARNING:', 'Oops:', 'general protection fault', 'Kernel panic'))
assert not any(line.startswith('swiftbaton_k ') for line in s.splitlines()), 'Module still loaded'
print('SBK_VM_LOG_VERIFIED', sys.argv[1])
PY
