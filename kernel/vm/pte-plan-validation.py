#!/usr/bin/env python3
from pathlib import Path
import argparse, hashlib, json, shutil, subprocess, time
p=argparse.ArgumentParser(description="Isolated detached PTE plan validation")
p.add_argument('--kernel',required=True); p.add_argument('--output',required=True)
a=p.parse_args(); K=Path(a.kernel).resolve(); S=Path(a.output).resolve()
S.mkdir(parents=True,exist_ok=True)
T=Path(__file__).resolve().parents[1]/'tests/pte-plan'
M=S/'module'; M.mkdir(exist_ok=True)
assert 'CONFIG_SWIFTBATON_PTE_PLAN_TEST=y' in (K/'.config').read_text()
for f in T.iterdir():
    if f.suffix in ('.c','.h') or f.name=='Makefile': shutil.copy2(f,M/f.name)
with (S/'module-build.log').open('w') as log:
    subprocess.run(['make','-j8','-C',str(M),'KDIR='+str(K)],stdout=log,stderr=subprocess.STDOUT,check=True)
root=S/'root'
for d in ['bin','proc','sys','dev','tmp']:(root/d).mkdir(parents=True,exist_ok=True)
shutil.copy2('/bin/busybox',root/'bin/busybox')
shutil.copy2(M/'plan-test-module.ko',root/'plan-test-module.ko')
subprocess.run(['gcc','-static','-O2','-Wall','-Wextra','-Werror',str(T/'plan-test.c'),'-o',str(root/'plan-test')],check=True)
(root/'init').write_text('''#!/bin/busybox sh
/bin/busybox --install -s /bin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
mount -t cgroup2 none /sys/fs/cgroup || { echo CGROUP_FAIL; poweroff -f; }
echo +memory > /sys/fs/cgroup/cgroup.subtree_control || { echo MEMORY_CONTROLLER_FAIL; poweroff -f; }
echo SBK_PLAN_VM_BOOT
uname -a
insmod /plan-test-module.ko || { echo MODULE_FAIL; poweroff -f; }
/plan-test
echo SBK_PLAN_TEST_EXIT=$?
unloaded=0
for i in $(seq 1 100); do
    if rmmod plan_test_module 2>/dev/null; then unloaded=1; break; fi
    sleep 0.1
done
echo SBK_PLAN_UNLOADED=$unloaded
echo SBK_TAINT=$(cat /proc/sys/kernel/tainted)
echo SBK_PLAN_VM_DONE
poweroff -f
''')
(root/'init').chmod(0o755)
with (S/'initrd.gz').open('wb') as f:
    subprocess.run(['bash','-o','pipefail','-c','find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip -1'],cwd=root,stdout=f,check=True)
log=S/('validation-'+time.strftime('%Y%m%d_%H%M%S')+'.log')
with log.open('wb') as f:
    result=subprocess.run(['sudo','-n','timeout','240','qemu-system-x86_64','-enable-kvm','-cpu','host','-m','4096','-smp','4',
        '-nodefaults','-nographic','-monitor','none','-serial','stdio','-no-reboot','-kernel',str(K/'arch/x86/boot/bzImage'),
        '-initrd',str(S/'initrd.gz'),'-append','console=ttyS0 panic=-1 init=/init nokaslr'],stdout=f,stderr=subprocess.STDOUT)
raw=log.read_text(); run=raw.split('SBK_PLAN_VM_BOOT',1)[-1]
markers=['SBK_PLAN_TEST_EXIT=0','SBK_PLAN_ALL_PASS','SBK_PLAN_UNLOADED=1','SBK_TAINT=12288','SBK_PLAN_VM_DONE']
missing=[s for s in markers if s not in run]
bad=[s for s in ['BUG:','WARNING:','Oops:','Kernel panic','general protection fault','KASAN:'] if s in run]
out={'passed':result.returncode==0 and not missing and not bad,'log':str(log),'missing':missing,'bad':bad,'returncode':result.returncode,
     'sha256':{str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in [K/'arch/x86/boot/bzImage',M/'plan-test-module.ko',root/'plan-test']}}
(S/'result.json').write_text(json.dumps(out,indent=2)+'\n');print(json.dumps(out,indent=2))
for line in run.splitlines():
    if 'SBK_PLAN_' in line:print(line)
assert out['passed']
