#!/usr/bin/env python3
"""Validate ARM bridge changes in an isolated kernel. Never modifies host modules."""
from pathlib import Path
import argparse,os,shutil,subprocess,hashlib,json,time
p=argparse.ArgumentParser();p.add_argument('--kernel',required=True);p.add_argument('--ofed');p.add_argument('--output',required=True);p.add_argument('--bench',action='store_true');a=p.parse_args()
R=Path(__file__).resolve().parents[1];K=Path(a.kernel).resolve();O=Path(a.output).resolve();O.mkdir(parents=True,exist_ok=True)
M=O/'module';M.mkdir(exist_ok=True);(O/'include').mkdir(exist_ok=True)
for src in (R/'include').glob('*.h'):shutil.copy2(src,O/'include'/src.name)
for src in (R/'module').iterdir():
 if src.suffix in ('.c','.h') or src.name=='Makefile':shutil.copy2(src,M/src.name)
args=['make','-j8','-C',str(M),'KDIR='+str(K)]
if a.ofed:args+=['OFA_DIR='+str(Path(a.ofed).resolve())]
with (O/'module-build.log').open('w') as f:subprocess.run(args,stdout=f,stderr=subprocess.STDOUT,check=True)
BM=O/'bridge-module';BM.mkdir(exist_ok=True)
for src in (R/'tests/bridge').iterdir():shutil.copy2(src,BM/src.name)
with (O/'bridge-build.log').open('w') as f:subprocess.run(['make','-j4','-C',str(K),'M='+str(BM),'modules'],stdout=f,stderr=subprocess.STDOUT,check=True)
root=O/'root'
for name in ['bin','proc','sys','dev','tmp']:(root/name).mkdir(parents=True,exist_ok=True)
shutil.copy2('/bin/busybox',root/'bin/busybox');shutil.copy2(M/'swiftbaton_k.ko',root/'swiftbaton_k.ko');shutil.copy2(BM/'sbk_bridge_test.ko',root/'sbk_bridge_test.ko')
for name,source in [('bridge-test',R/'tests/bridge/bridge-test.c'),('sbk_test',R/'tests/sbk_test.c'),('arm-bench',R/'tests/arm-bench.c')]:
 subprocess.run(['gcc','-static','-O2','-Wall','-Wextra','-pthread','-I'+str(R/'include'),str(source),'-o',str(root/name)],check=True)
init='''#!/bin/busybox sh
/bin/busybox --install -s /bin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
echo SBK_ARM_VALIDATION_BOOT
uname -a
sh /load-modules.sh || { echo DEPENDENCY_FAIL; poweroff -f; }
insmod /sbk_bridge_test.ko || { echo BRIDGE_MODULE_FAIL; poweroff -f; }
/bridge-test
echo SBK_BRIDGE_EXIT=$?
rmmod sbk_bridge_test
echo SBK_BRIDGE_UNLOAD_EXIT=$?
'''
if not a.ofed:init+='''/usr/sbin/ip link set lo up
/usr/sbin/ip link set dummy0 up
/usr/sbin/ip addr add 192.168.234.1/24 dev dummy0
/usr/bin/rdma link add rxe0 type rxe netdev dummy0 || { echo RXE_FAIL; poweroff -f; }
export SBK_TEST_RDMA=1
'''
init+='''insmod /swiftbaton_k.ko || { echo MODULE_FAIL; poweroff -f; }
SBK_TEST_ANON=1 SBK_TEST_TOKEN_POOL=1 /sbk_test
echo SBK_FULL_TEST_EXIT=$?
'''
if a.bench:init+='''/arm-bench 16384
echo SBK_SMALL_EXIT=$?
/arm-bench 262144
echo SBK_LARGE_EXIT=$?
'''
init+='''unloaded=0
for i in $(seq 1 200); do
 if rmmod swiftbaton_k 2>/dev/null; then unloaded=1; break; fi
 sleep 0.1
done
echo SBK_UNLOADED=$unloaded
echo SBK_TAINT=$(cat /proc/sys/kernel/tainted)
echo SBK_ARM_VALIDATION_DONE
poweroff -f
'''
(root/'init').write_text(init);(root/'init').chmod(0o755)
env=dict(os.environ,KDIR=str(K),SBK_VM_ROOT=str(root));env.pop('SBK_OFED_ROOT',None)
if a.ofed:env['SBK_OFED_ROOT']=str(Path(a.ofed).resolve())
subprocess.run(['python3',str(R/'vm/prepare.py')],env=env,check=True)
with (O/'initrd.gz').open('wb') as f:subprocess.run(['bash','-c','find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip -1'],cwd=root,stdout=f,check=True)
log=O/('validation-'+time.strftime('%Y%m%d_%H%M%S')+'.log')
with log.open('wb') as f:subprocess.run(['sudo','-n','timeout','240','qemu-system-x86_64','-enable-kvm','-cpu','host','-m','4096','-smp','4','-nodefaults','-nographic','-monitor','none','-serial','stdio','-no-reboot','-kernel',str(K/'arch/x86/boot/bzImage'),'-initrd',str(O/'initrd.gz'),'-append','console=ttyS0 panic=-1 init=/init nokaslr'],stdout=f,stderr=subprocess.STDOUT,check=True)
raw=log.read_text();runtime=raw.split('SBK_ARM_VALIDATION_BOOT',1)[1]
markers=['SBK_BRIDGE_TEST_PASS','SBK_BRIDGE_EXIT=0','SBK_BRIDGE_UNLOAD_EXIT=0','SBK_TOKEN_POOL_PASS','SBK_VM_TESTS_PASS','SBK_FULL_TEST_EXIT=0','SBK_UNLOADED=1','SBK_TAINT=12288','SBK_ARM_VALIDATION_DONE']
if not a.ofed:markers+=['SBK_VM_RDMA_TESTS_PASS']
if a.bench:markers+=['SBK_SMALL_EXIT=0','SBK_LARGE_EXIT=0']
for marker in markers:assert marker in runtime,marker
assert not any(x in runtime for x in ['BUG:','WARNING:','Oops:','Kernel panic','general protection fault'])
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
result=dict(passed=True,host_loaded=False,kernel=str(K),kernel_sha256=sha(K/'arch/x86/boot/bzImage'),bridge_source_sha256=sha(K/'mm/swiftbaton_pte.c'),module_sha256=sha(M/'swiftbaton_k.ko'),log=str(log),ofed=bool(a.ofed),rxe=not a.ofed)
(O/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
for line in runtime.splitlines():
 if 'SBK_ARM_BENCH' in line:print(line)
