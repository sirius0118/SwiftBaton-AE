#!/usr/bin/env python3
"""Copy source and locally compiled files to peers; preview unless --execute."""
from pathlib import Path
import argparse,fcntl,json,os,shlex,subprocess
R=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--execute',action='store_true');a=p.parse_args()
commands=[]
for h in ('knode1','knode3'):
 commands.append(['rsync','-az','--exclude=.git/','--exclude=/build/','--exclude=/.venv/','--exclude=__pycache__/','--exclude=.DS_Store','-e','ssh -oBatchMode=yes',str(R)+'/',h+':'+str(R)+'/'])
 for mode in ('U','K'):
  commands.append(['rsync','-az','--rsync-path=mkdir -p '+shlex.quote(str(R/'build'/('criu-'+mode)/'criu'))+' && rsync',str(R/'build'/('criu-'+mode)/'criu/criu'),h+':'+str(R/'build'/('criu-'+mode)/'criu/criu')])
 commands.append(['rsync','-az','--rsync-path=mkdir -p '+shlex.quote(str(R/'build/YCSB'))+' && rsync',str(R/'build/YCSB')+'/',h+':'+str(R/'build/YCSB')+'/'])
 if (R/'build/fixture/memory_fixture').exists():commands.append(['rsync','-az','--rsync-path=mkdir -p '+shlex.quote(str(R/'build/fixture'))+' && rsync',str(R/'build/fixture/memory_fixture'),h+':'+str(R/'build/fixture/memory_fixture')])
for c in commands:print(shlex.join(c),flush=True)
if not a.execute:print('PREVIEW ONLY');raise SystemExit(0)
for f in ('build/criu-U/criu/criu','build/criu-K/criu/criu','build/YCSB/core/target/classes/site/ycsb/Client.class','build/YCSB/redis/target/classes/site/ycsb/db/RedisClient.class'):
 if not (R/f).is_file():raise SystemExit('Build before staging: missing '+f)
W=Path(os.environ.get('SB_AE_WORK_ROOT',str(R.parent/(R.name+'-work')))).resolve();W.mkdir(parents=True,exist_ok=True)
lock=(W/'run.lock').open('w');fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
probe="import subprocess; p=subprocess.run(['pgrep','-x','criu'],capture_output=True); assert p.returncode==1,'Active CRIU'; assert not subprocess.check_output(['docker','ps','-aq','--filter','label=swiftbaton.ae=true'],text=True).strip(),'Retained AE containers'"
for h in ('knode2','knode3'):
 cmd=['sudo','-n','python3','-c',probe]
 if h!='knode2':cmd=['ssh','-oBatchMode=yes',h,shlex.join(cmd)]
 subprocess.run(cmd,check=True,timeout=30)
for h in ('knode1','knode3'):
 guard="from pathlib import Path;p=Path("+repr(str(R))+");assert not p.exists() or not any(p.iterdir()) or ((p/'README.md').exists() and 'SwiftBaton' in (p/'README.md').read_text()),'Unrelated destination directory';p.mkdir(parents=True,exist_ok=True)"
 subprocess.run(['ssh','-oBatchMode=yes',h,shlex.join(['python3','-c',guard])],check=True,timeout=30)
for cmd in commands:subprocess.run(cmd,check=True)
print('Staged; installed CRIU symlinks and running services are unchanged.')
