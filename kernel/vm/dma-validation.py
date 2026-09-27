#!/usr/bin/env python3
"""Validate the opt-in DMA-MR transport in an isolated native RXE/KASAN guest."""
from pathlib import Path
import argparse, hashlib, json, os, shutil, subprocess, time
p=argparse.ArgumentParser()
p.add_argument('--kernel',required=True); p.add_argument('--output',required=True)
p.add_argument('--retirement-audit',action='store_true')
p.add_argument('--defer-creator-drop',action='store_true')
a=p.parse_args(); R=Path(__file__).resolve().parents[1]; K=Path(a.kernel).resolve()
O=Path(a.output).resolve(); O.mkdir(parents=True,exist_ok=True)
M=O/'module'; M.mkdir(exist_ok=True); (O/'include').mkdir(exist_ok=True)
for f in (R/'include').glob('*.h'): shutil.copy2(f,O/'include'/f.name)
for f in (R/'module').iterdir():
    if f.suffix in ('.c','.h') or f.name=='Makefile': shutil.copy2(f,M/f.name)
with (O/'module-build.log').open('w') as log:
    subprocess.run(['make','-j8','-C',str(M),'KDIR='+str(K)],stdout=log,stderr=subprocess.STDOUT,check=True)
root=O/'root'
for name in ['bin','proc','sys','dev','tmp']: (root/name).mkdir(parents=True,exist_ok=True)
shutil.copy2('/bin/busybox',root/'bin/busybox'); shutil.copy2(M/'swiftbaton_k.ko',root/'swiftbaton_k.ko')
subprocess.run(['gcc','-static','-O2','-Wall','-Wextra','-Werror','-pthread','-I'+str(R/'include'),
    str(R/'tests/sbk_test.c'),'-o',str(root/'sbk_test')],check=True)
init='''#!/bin/busybox sh
/bin/busybox --install -s /bin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
echo SBK_DMA_VM_BOOT
uname -a
sh /load-modules.sh || { echo DEPENDENCY_FAIL; poweroff -f; }
/usr/sbin/ip link set lo up
/usr/sbin/ip link set dummy0 up
/usr/sbin/ip addr add 192.168.234.1/24 dev dummy0
/usr/bin/rdma link add rxe0 type rxe netdev dummy0 || { echo RXE_FAIL; poweroff -f; }
insmod /swiftbaton_k.ko rdma_debug=1 || { echo MODULE_FAIL; poweroff -f; }
SBK_TEST_DMA_ONLY=1 /sbk_test
echo SBK_DMA_FILE_EXIT=$?
SBK_TEST_DMA_ONLY=1 SBK_TEST_ANON=1 SBK_TEST_TOKEN_POOL=1 /sbk_test
echo SBK_DMA_ANON_EXIT=$?
export SBK_PEER_SOURCE_IP=192.168.234.1
for mode in dma ordinary; do
    unset SBK_TEST_DMA_ORDINARY
    if [ "$mode" = ordinary ]; then export SBK_TEST_DMA_ORDINARY=1; fi
    /sbk_test --dma-peer-source & peer=$!
    SBK_TEST_ANON=1 SBK_TEST_TOKEN_POOL=1 /sbk_test --dma-peer-destination
    echo SBK_DMA_PEER_${mode}_DEST_EXIT=$?
    wait $peer
    echo SBK_DMA_PEER_${mode}_SOURCE_EXIT=$?
done
unset SBK_TEST_DMA_ORDINARY
SBK_FAULT_PROBE=1 /sbk_test --dma-peer-source & peer=$!
SBK_FAULT_PROBE=1 SBK_TEST_ANON=1 SBK_TEST_TOKEN_POOL=1 /sbk_test --dma-peer-destination
echo SBK_DMA_PROBE_DEST_EXIT=$?
wait $peer
echo SBK_DMA_PROBE_SOURCE_EXIT=$?
SBK_TEST_RDMA=1 SBK_TEST_ANON=1 SBK_TEST_TOKEN_POOL=1 /sbk_test
echo SBK_REGRESSION_EXIT=$?
unloaded=0
for i in $(seq 1 200); do
    if rmmod swiftbaton_k 2>/dev/null; then unloaded=1; break; fi
    sleep 0.1
done
echo SBK_UNLOADED=$unloaded
echo SBK_TAINT=$(cat /proc/sys/kernel/tainted)
echo SBK_DMA_VM_DONE
poweroff -f
'''
if a.retirement_audit: init=init.replace('rdma_debug=1 ||', 'rdma_debug=1 retirement_audit=1 arm_timing=1 ||')
if a.defer_creator_drop:
    init=init.replace('insmod /swiftbaton_k.ko rdma_debug=1', 'insmod /swiftbaton_k.ko defer_creator_drop=1 arm_timing=1 rdma_debug=1')
    init=init.replace('echo SBK_TAINT=', '''insmod /swiftbaton_k.ko defer_creator_drop=1 creator_drop_test_delay_ms=1000 || { echo CREATOR_MODULE_FAIL; poweroff -f; }
SBK_TEST_CREATOR_LIFETIME=1 SBK_TEST_ANON=1 SBK_TEST_TOKEN_POOL=1 /sbk_test
echo SBK_CREATOR_LIFETIME_EXIT=$?
unloaded=0
for i in $(seq 1 200); do
    if rmmod swiftbaton_k 2>/dev/null; then unloaded=1; break; fi
    sleep 0.1
done
echo SBK_CREATOR_UNLOADED=$unloaded
echo SBK_TAINT=''')
(root/'init').write_text(init); (root/'init').chmod(0o755)
env=dict(os.environ,KDIR=str(K),SBK_VM_ROOT=str(root)); env.pop('SBK_OFED_ROOT',None)
subprocess.run(['python3',str(R/'vm/prepare.py')],env=env,check=True)
with (O/'initrd.gz').open('wb') as f:
    subprocess.run(['bash','-o','pipefail','-c','find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip -1'],
        cwd=root,stdout=f,check=True)
log=O/('validation-'+time.strftime('%Y%m%d_%H%M%S')+'.log')
with log.open('wb') as f:
    result=subprocess.run(['sudo','-n','timeout','240','qemu-system-x86_64','-enable-kvm','-cpu','host','-m','4096',
        '-smp','4','-nodefaults','-nographic','-monitor','none','-serial','stdio','-no-reboot',
        '-kernel',str(K/'arch/x86/boot/bzImage'),'-initrd',str(O/'initrd.gz'),
        '-append','console=ttyS0 panic=-1 init=/init nokaslr'],stdout=f,stderr=subprocess.STDOUT)
raw=log.read_text(); runtime=raw.split('SBK_DMA_VM_BOOT',1)[-1]
markers=['SBK_DMA_FILE_EXIT=0','SBK_DMA_ANON_EXIT=0','SBK_REGRESSION_EXIT=0','SBK_VM_RDMA_TESTS_PASS',
    'SBK_UNLOADED=1','SBK_TAINT=12288','SBK_DMA_VM_DONE']
markers += ['SBK_DMA_PEER_'+mode+'_'+role+'_EXIT=0' for mode in ['dma','ordinary'] for role in ['DEST','SOURCE']]
markers += ['SBK_DMA_PROBE_DEST_EXIT=0','SBK_DMA_PROBE_SOURCE_EXIT=0']
if a.retirement_audit: markers += ['SBK_RETIRE_UNFETCHED','SBK_ARM pages=']
if a.defer_creator_drop: markers += ['deferred=1','SBK_CREATOR_DROP pages=',
    'SBK_CREATOR_LIFETIME_EXIT=0','SBK_CREATOR_LIFETIME_PASS','SBK_CREATOR_UNLOADED=1']
missing=[x for x in markers if x not in runtime]
bad=[x for x in ['BUG:','WARNING:','Oops:','Kernel panic','general protection fault'] if x in runtime]
def sha(f): return hashlib.sha256(f.read_bytes()).hexdigest()
out=dict(passed=result.returncode==0 and not missing and not bad,host_loaded=False,rxe=True,
    retirement_audit=a.retirement_audit,
    defer_creator_drop=a.defer_creator_drop,
    kernel_sha256=sha(K/'arch/x86/boot/bzImage'),module_sha256=sha(M/'swiftbaton_k.ko'),
    test_sha256=sha(root/'sbk_test'),log=str(log),returncode=result.returncode,missing=missing,kernel_errors=bad)
(O/'result.json').write_text(json.dumps(out,indent=2)+'\n'); print(json.dumps(out,indent=2))
for line in runtime.splitlines():
    if 'PASS DMA_' in line or 'FAIL ' in line: print(line)
if not out['passed']: raise SystemExit(1)
