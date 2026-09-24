#!/usr/bin/env python3
"""Build the patched kernel in an isolated tree. Does not install or reboot."""
from pathlib import Path
import argparse,hashlib,json,subprocess
p=argparse.ArgumentParser(description=__doc__);p.add_argument('source',type=Path);p.add_argument('--jobs',type=int,default=8);a=p.parse_args()
r=Path(__file__).resolve().parents[1];src=a.source.resolve();dst=r/'build/linux-5.15.167-swiftbaton-k1'
if not 1<=a.jobs<=80:raise SystemExit('jobs must be 1..80')
patch=r/'kernel/patches/linux-5.15.167-sbk-pte.patch';marker=dst/'.swiftbaton-source.json'
id={'source':str(src),'patch_sha256':hashlib.sha256(patch.read_bytes()).hexdigest()}
if not marker.exists():
 if dst.exists():raise SystemExit('Build tree already exists without a matching marker; inspect it before reuse')
 version=subprocess.check_output(['make','-s','-C',str(src),'kernelversion'],text=True).strip()
 if version!='5.15.167':raise SystemExit('Requires Linux 5.15.167 source')
 with patch.open('rb') as f:subprocess.run(['patch','--dry-run','-p1','--batch','--forward'],cwd=src,stdin=f,check=True)
 dst.mkdir(parents=True)
 excludes=['.git/','*.o','*.ko','*.a','.*.cmd','*.mod','*.mod.c','Module.symvers','modules.order','/vmlinux','/System.map','/arch/x86/boot/bzImage','/certs/signing_key*','.sbk-*']
 subprocess.run(['rsync','-a']+['--exclude='+x for x in excludes]+[str(src)+'/',str(dst)+'/'],check=True)
 with patch.open('rb') as f:subprocess.run(['patch','-p1','--batch','--forward'],cwd=dst,stdin=f,check=True)
 (dst/'.config').write_bytes((r/'kernel/config-5.15.167-swiftbaton-k1').read_bytes());marker.write_text(json.dumps(id))
else:
 if json.loads(marker.read_text())!=id:raise SystemExit('Source or patch changed; use a fresh build directory')
subprocess.run(['make','olddefconfig'],cwd=dst,check=True)
release=subprocess.check_output(['make','-s','kernelrelease'],cwd=dst,text=True).strip()
if release!='5.15.167-swiftbaton-k1':raise SystemExit('Unexpected kernel release '+release)
subprocess.run(['make','-j'+str(a.jobs),'bzImage','modules'],cwd=dst,check=True)
print('Built '+release+' at '+str(dst)+'; nothing installed.')
