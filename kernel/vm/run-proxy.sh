#!/usr/bin/env bash
set -euo pipefail
sbk_root=$(cd "$(dirname "$0")/.." && pwd)
kernel_root=${KDIR:-/home/k8s/exper/zxz/live_migration/SwiftBaton-K/build/linux-5.15.167-sbk-validate}
root="$sbk_root/vm/proxy-root"
gcc -static -O2 -pthread -I"$sbk_root/include" \
    "$sbk_root/tests/sbk_proxy_test.c" -o "$sbk_root/tests/sbk_proxy_test"
mkdir -p "$root"/{bin,proc,sys,dev,tmp}
cp /bin/busybox "$root/bin/busybox"
cp "$sbk_root/module/swiftbaton_k.ko" "$root/swiftbaton_k.ko"
cp "$sbk_root/tests/sbk_proxy_test" "$root/sbk_proxy_test"
cp "$sbk_root/vm/init-proxy" "$root/init"
chmod +x "$root/init"
SBK_VM_ROOT="$root" KDIR="$kernel_root" python3 "$sbk_root/vm/prepare.py"
(cd "$root" && find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip -1 > ../proxy-initrd.gz)
log="$sbk_root/vm/proxy-$(date +%Y%m%d_%H%M%S).log"
sudo -n timeout 180 qemu-system-x86_64 -enable-kvm -cpu host -m 2048 -smp 4 \
    -nodefaults -nographic -monitor none -serial stdio -no-reboot \
    -kernel "$kernel_root/arch/x86/boot/bzImage" -initrd "$sbk_root/vm/proxy-initrd.gz" \
    -append 'console=ttyS0 panic=-1 init=/init nokaslr' > "$log" 2>&1
tail -n 35 "$log"
python3 - "$log" <<'PY'
import pathlib, sys
s = pathlib.Path(sys.argv[1]).read_text()
for marker in ('SBK_PROXY_VM_BOOT', 'SBK_PROXY_FILE_PASS', 'SBK_PROXY_ANON_PASS',
               'SBK_PROXY_CANCEL_PASS',
               'SBK_PROXY_INFLIGHT_CANCEL_PASS',
               'SBK_PROXY_TESTS_PASS', 'SBK_PROXY_EXIT=0', 'SBK_PROXY_VM_DONE'):
    assert marker in s, marker
test_output = s.split('SBK_PROXY_VM_BOOT', 1)[1]
assert not any(x in test_output for x in ('BUG:', 'WARNING:', 'Oops:', 'Kernel panic',
                                'SBK_PROXY_UNLOAD_FAIL'))
print('SBK_PROXY_VM_LOG_VERIFIED', sys.argv[1])
PY
