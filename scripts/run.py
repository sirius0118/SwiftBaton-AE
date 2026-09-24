#!/usr/bin/env python3
"""Run one U or K migration on the prepared cluster; default is a local preview."""
from pathlib import Path
import argparse,fcntl,hashlib,json,os,re,shlex,subprocess,sys,time
R=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('mode',choices=['U','K']);p.add_argument('--profile',choices=['smoke','redis'],default='smoke')
g=p.add_mutually_exclusive_group();g.add_argument('--check',action='store_true');g.add_argument('--execute',action='store_true')
a=p.parse_args()
W=Path(os.environ.get('SB_AE_WORK_ROOT',str(R.parent/(R.name+'-work')))).resolve()
if R==W or R in W.parents:raise SystemExit('SB_AE_WORK_ROOT must be outside the source repository')
profile=json.loads((R/'configs/profiles.json').read_text())[a.mode][:]
if a.profile=='smoke':
 for key,value in [('--records','100000'),('--field-length','1024'),('--duration','45'),('--warmup','10'),('--threads','16')]:profile[profile.index(key)+1]=value
argv=[sys.executable,str(R/'scripts/ae'/a.mode.lower()/'run_ae.py')]+profile
if not (a.execute or a.check):
 print(json.dumps(dict(mode=a.mode,profile=a.profile,command=argv,results=str(W),mutates_hosts=False),indent=2));sys.exit(0)
W.mkdir(parents=True,exist_ok=True)
lock=(W/'run.lock').open('w');fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
binary=R/'build'/('criu-'+a.mode)/'criu/criu'
sha=hashlib.sha256(binary.read_bytes()).hexdigest()
def remote(host,program,args=()):
 command=['sudo','-n','python3','-c',program]+list(map(str,args))
 if host!='knode2':command=['ssh','-oBatchMode=yes','-oConnectTimeout=8',host,shlex.join(command)]
 return subprocess.check_output(command,text=True,timeout=45)
probe=r'''from pathlib import Path
import os,json,hashlib,subprocess,sys
binary,expected,mode=sys.argv[1:]
active=[]
for p in Path('/proc').iterdir():
 if p.name.isdigit():
  try:
   if (p/'comm').read_text().strip()=='criu':active.append(p.name)
  except FileNotFoundError:pass
assert not active,('Active CRIU tasks',active)
assert not subprocess.check_output(['docker','ps','-aq','--filter','label=swiftbaton.ae=true'],text=True).strip(),'Clean the existing owned AE containers first'
assert hashlib.sha256(Path(binary).read_bytes()).hexdigest()==expected,'Rebuild and stage matching CRIU on both hosts'
p=Path('/usr/bin/criu');assert p.is_symlink(),'Administrator must provision /usr/bin/criu as a symlink first'
assert p.is_file(),'Installed CRIU symlink is broken'
ref=Path('/sys/module/swiftbaton_k/refcnt')
if ref.exists():assert ref.read_text().strip()=='0','K module is busy'
if mode=='K':
 assert ref.exists(),'The reviewed K module must already be loaded on both hosts'
 assert Path('/sys/module/swiftbaton_k/parameters/session_dispatch').read_text().strip()=='Y'
print(json.dumps(dict(installed=os.readlink(p),kernel=os.uname().release)))
'''
switch=r'''from pathlib import Path
import os,sys,uuid
before,target=sys.argv[1:];p=Path('/usr/bin/criu')
assert p.is_symlink() and os.readlink(p)==before,'CRIU selection changed concurrently'
tmp=p.parent/('.swiftbaton-criu-'+uuid.uuid4().hex);os.symlink(target,tmp)
try:os.replace(tmp,p)
finally:
 if tmp.is_symlink():tmp.unlink()
'''
check_paths=r'''from pathlib import Path
import os,sys,json,shutil,hashlib
expected=sys.argv[1];seen=[]
def check(p):
 assert p and hashlib.sha256(Path(p).read_bytes()).hexdigest()==expected,('Wrong CRIU path',p)
 seen.append(p)
check('/usr/bin/criu');check(shutil.which('criu'))
daemons=set()
for p in Path('/proc').iterdir():
 if not p.name.isdigit():continue
 try:n=(p/'comm').read_text().strip()
 except FileNotFoundError:continue
 if n not in ('dockerd','containerd'):continue
 e=dict(x.split(b'=',1) for x in (p/'environ').read_bytes().split(b'\0') if b'=' in x)
 check(shutil.which('criu',path=e.get(b'PATH',b'').decode()));daemons.add(n)
assert daemons=={'dockerd','containerd'}
print(json.dumps(seen))
'''
previous={h:json.loads(remote(h,probe,[binary,sha,a.mode])) for h in ('knode2','knode3')}
# Check client classes before creating any workload.
client=r'''from pathlib import Path
import subprocess,json,sys
r=Path(sys.argv[1]);names=['core/target/classes/site/ycsb/Client.class','redis/target/classes/site/ycsb/db/RedisClient.class']
assert all((r/'build/YCSB'/n).is_file() for n in names),'Build and stage YCSB first'
assert list((r/'build/YCSB/core/target/dependency').glob('*.jar')),'Missing YCSB runtime dependencies'
print(json.dumps(names))
'''
remote('knode1',client,[R])
image_ref=os.environ.get('SB_REDIS_IMAGE',json.loads((R/'configs/lab.json').read_text())['redis_image'])
image_ids={}
for host in previous:
 image_ids[host]=remote(host,"import subprocess,sys;print(subprocess.check_output(['docker','image','inspect','--format','{{.Id}}',sys.argv[1]],text=True).strip())",[image_ref]).strip()
if len(set(image_ids.values()))!=1:raise SystemExit('Source and destination Redis images differ')
if a.check:print(json.dumps(dict(ok=True,mode=a.mode,hosts=previous,binary_sha256=sha),indent=2));sys.exit(0)
out=W/('driver-'+a.mode+'-'+time.strftime('%Y%m%d_%H%M%S'));out.mkdir()
result=dict(mode=a.mode,profile=a.profile,argv=argv,binary_sha256=sha,previous=previous,selected=str(binary),success=False);changed=[];state=None
# Persist rollback information before changing either installed symlink.
(out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
try:
 for host in previous:
  remote(host,switch,[previous[host]['installed'],binary]);changed.append(host)
  result['changed_hosts']=changed[:]
  (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
  remote(host,check_paths,[sha])
 with (out/'run.log').open('w') as log:
  env=dict(os.environ,SB_AE_WORK_ROOT=str(W),SB_REDIS_IMAGE=image_ref,TZ='Asia/Shanghai')
  proc=subprocess.Popen(argv,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
  for line in proc.stdout:
   log.write(line);log.flush();print(line,end='',flush=True)
   if line.startswith('STATE='):state=Path(line.strip().split('=',1)[1]);result['state']=str(state)
  result['driver_rc']=proc.wait()
 if result['driver_rc'] or state is None or not json.loads(state.read_text()).get('success'):raise RuntimeError('Migration failed; inspect '+str(out/'run.log'))
 scripts=['analyze_run.py','analyze_recovery.py','verify_images.py']
 if a.mode=='U':scripts+=['analyze_transport.py','analyze_faults.py']
 result['analysis']={}
 for name in scripts:
  with (out/(name+'.log')).open('w') as log:
   q=subprocess.run([sys.executable,str(R/'scripts'/name),str(state if name=='verify_images.py' else state.parent)],stdout=log,stderr=subprocess.STDOUT)
  result['analysis'][name]=q.returncode
  if q.returncode:raise RuntimeError('Validation failed: '+name)
 result['success']=True
finally:
 if state:
  with (out/'cleanup.log').open('w') as log:q=subprocess.run([sys.executable,str(R/'scripts/ae'/a.mode.lower()/'cleanup_ae.py'),str(state)],stdout=log,stderr=subprocess.STDOUT)
  result['cleanup_rc']=q.returncode
  if q.returncode:result['success']=False
 result['restored']={}
 for host in reversed(changed):
  try:
   remote(host,probe,[binary,sha,a.mode]);remote(host,switch,[binary,previous[host]['installed']]);result['restored'][host]=True
  except Exception as e:result['restored'][host]=str(e);result['success']=False
 (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
 print('RESULT='+str(out/'result.json'),flush=True)
if not result['success']:raise SystemExit(1)
print('SWIFTBATON_AE_PASS mode='+a.mode)
