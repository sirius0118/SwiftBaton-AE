#!/usr/bin/env python3
"""Run one baseline algorithm profile on the prepared cluster; default is preview."""
from pathlib import Path
import argparse,fcntl,hashlib,json,os,re,shlex,subprocess,sys,time
R=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('baseline',choices=['pclive','postcopy','hybrid','remote-fork']);p.add_argument('--profile',choices=['smoke','redis'],default='smoke')
p.add_argument('--duration',type=int,help='YCSB run seconds, including migration')
p.add_argument('--warmup',type=int,help='seconds before starting migration')
p.add_argument('--threads',type=int,help='YCSB client threads')
g=p.add_mutually_exclusive_group();g.add_argument('--check',action='store_true');g.add_argument('--execute',action='store_true')
p.add_argument('--network-lock', choices=['iptables','nftables'])
p.add_argument('--vma-cache', action='store_true')
p.add_argument('--buffered-cutover', action='store_true')
p.add_argument('--stage-max-mb', type=int)
p.add_argument('--precopy-limit-mb', type=int)
p.add_argument('--kernel-export-workers', type=int, choices=range(1,33))
p.add_argument('--kernel-catalog-workers', type=int, choices=range(1,33))
p.add_argument('--kernel-dma-mr', action='store_true')
p.add_argument('--kernel-ps-arm', action='store_true')
p.add_argument('--kernel-ps-mr', action='store_true')
p.add_argument('--kernel-ps-mr-all', action='store_true')
p.add_argument('--kernel-export-chunk-mb', type=int)
p.add_argument('--validation-workers', type=int, choices=range(1,33))
a=p.parse_args()
criu_mode='K' if a.baseline=='remote-fork' else 'U'
W=Path(os.environ.get('SB_AE_WORK_ROOT',str(R.parent/(R.name+'-work')))).resolve()
if R==W or R in W.parents:raise SystemExit('SB_AE_WORK_ROOT must be outside the source repository')
profile=json.loads((R/'baseline/profiles.json').read_text())[a.baseline][:]
if a.profile=='smoke':
 for key,value in [('--records','100000'),('--field-length','1024'),('--duration','45'),('--warmup','10'),('--threads','16')]:profile[profile.index(key)+1]=value
for key,value in (('--duration',a.duration),('--warmup',a.warmup),('--threads',a.threads)):
 if value is not None:profile[profile.index(key)+1]=str(value)
if int(profile[profile.index('--duration')+1]) < int(profile[profile.index('--warmup')+1])+20:raise SystemExit('duration must exceed warmup by at least 20 seconds')
if a.validation_workers is not None:
 if '--validation-workers' in profile:profile[profile.index('--validation-workers')+1]=str(a.validation_workers)
 else:profile += ['--validation-workers',str(a.validation_workers)]
for key,value in (('--kernel-catalog-workers',a.kernel_catalog_workers),('--kernel-export-workers',a.kernel_export_workers),('--kernel-export-chunk-mb',a.kernel_export_chunk_mb)):
 if value is not None:
  if criu_mode!='K':raise SystemExit(key+' requires K mode')
  if key=='--kernel-export-chunk-mb' and not 0<=value<=4096:raise SystemExit('kernel-export-chunk-mb must be 0..4096')
  if key in profile:profile[profile.index(key)+1]=str(value)
  else:profile += [key,str(value)]
if a.kernel_ps_arm:
 if criu_mode!='K':raise SystemExit('--kernel-ps-arm requires K mode')
 profile += ['--kernel-ps-arm']
if a.kernel_ps_mr:
 if criu_mode!='K' or not (a.kernel_ps_arm or '--kernel-ps-arm' in profile):raise SystemExit('--kernel-ps-mr requires K and --kernel-ps-arm')
 profile += ['--kernel-ps-mr']
if a.kernel_ps_mr_all:
 if not a.kernel_ps_mr:raise SystemExit('--kernel-ps-mr-all requires --kernel-ps-mr')
 profile += ['--kernel-ps-mr-all']
if a.kernel_dma_mr:
 if criu_mode!='K':raise SystemExit('--kernel-dma-mr requires K mode')
 profile += ['--kernel-dma-mr']
if a.precopy_limit_mb is not None:
 if not 1 <= a.precopy_limit_mb <= 65536:raise SystemExit('precopy-limit-mb must be 1..65536')
 if '--precopy-limit-mb' in profile:profile[profile.index('--precopy-limit-mb')+1]=str(a.precopy_limit_mb)
 else:profile += ['--precopy-limit-mb',str(a.precopy_limit_mb)]
if a.stage_max_mb is not None:profile += ['--stage-max-mb', str(a.stage_max_mb)]
if a.buffered_cutover:profile += ['--buffered-cutover']
if a.network_lock:profile += ['--network-lock',a.network_lock]
if a.vma_cache and '--vma-cache' not in profile:profile += ['--vma-cache']
argv=[sys.executable,str(R/'scripts/ae'/criu_mode.lower()/'run_ae.py')]+profile
if not (a.execute or a.check):
 print(json.dumps(dict(baseline=a.baseline,criu_mode=criu_mode,profile=a.profile,command=argv,results=str(W),mutates_hosts=False),indent=2));sys.exit(0)
W.mkdir(parents=True,exist_ok=True)
lock=(W/'run.lock').open('w');fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
binary=R/'build'/('criu-K-baseline' if a.baseline=='remote-fork' else
                  'criu-U-pclive' if a.baseline=='pclive' else 'criu-U')/'criu/criu'
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
previous={h:json.loads(remote(h,probe,[binary,sha,criu_mode])) for h in ('knode2','knode3')}
# Check client classes before creating any workload.
client=r'''from pathlib import Path
import subprocess,json,sys
r=Path(sys.argv[1]);names=['core/target/classes/site/ycsb/Client.class','redis/target/classes/site/ycsb/db/RedisClient.class']
assert all((r/'build/YCSB'/n).is_file() for n in names),'Build and stage YCSB first'
assert list((r/'build/YCSB/core/target/dependency').glob('*.jar')),'Missing YCSB runtime dependencies'
print(json.dumps(names))
'''
remote('knode1',client,[R])
if a.buffered_cutover:
 remote('knode1', "import ctypes;ctypes.CDLL('libnetfilter_queue.so.1');ctypes.CDLL('libnftables.so.1');ctypes.CDLL('libnetfilter_conntrack.so.3')")
if a.buffered_cutover:remote('knode3', "import ctypes;ctypes.CDLL('libnetfilter_conntrack.so.3')")
image_ref=os.environ.get('SB_REDIS_IMAGE',json.loads((R/'configs/lab.json').read_text())['redis_image'])
image_ids={}
for host in previous:
 image_ids[host]=remote(host,"import subprocess,sys;print(subprocess.check_output(['docker','image','inspect','--format','{{.Id}}',sys.argv[1]],text=True).strip())",[image_ref]).strip()
if len(set(image_ids.values()))!=1:raise SystemExit('Source and destination Redis images differ')
if a.check:print(json.dumps(dict(ok=True,baseline=a.baseline,criu_mode=criu_mode,hosts=previous,binary_sha256=sha),indent=2));sys.exit(0)
out=W/('baseline-'+a.baseline+'-'+time.strftime('%Y%m%d_%H%M%S'));out.mkdir()
result=dict(baseline=a.baseline,criu_mode=criu_mode,profile=a.profile,argv=argv,binary_sha256=sha,previous=previous,selected=str(binary),success=False);changed=[];state=None
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
 scripts=['verify_images.py','analyze_run.py','analyze_recovery.py','analyze_success_gaps.py']
 if criu_mode=='U' and '--parallel-transfer' in profile:
  scripts+=['analyze_transport.py']
  if '--fault-trace' in profile:scripts+=['analyze_faults.py']
 result['analysis']={}
 for name in scripts:
  with (out/(name+'.log')).open('w') as log:
   command=[sys.executable,str(R/'scripts'/name),str(state if name=='verify_images.py' else state.parent)]
   if a.baseline=='remote-fork' and name=='analyze_recovery.py':
    command+=['--reference-event','sentinel_verified']
   q=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT)
  result['analysis'][name]=q.returncode
 if a.baseline=='pclive':
  with (out/'verify_delta.py.log').open('w') as log:
   q=subprocess.run([sys.executable,str(R/'baseline/pclive/verify_delta.py'),str(state.parent)],
                    stdout=log,stderr=subprocess.STDOUT)
  result['analysis']['verify_delta.py']=q.returncode
 failed=[name for name,code in result['analysis'].items() if code]
 if failed:raise RuntimeError('Validation failed: '+', '.join(failed))
 result['success']=True
finally:
 if state:
  with (out/'cleanup.log').open('w') as log:q=subprocess.run([sys.executable,str(R/'scripts/ae'/criu_mode.lower()/'cleanup_ae.py'),str(state)],stdout=log,stderr=subprocess.STDOUT)
  result['cleanup_rc']=q.returncode
  if q.returncode:result['success']=False
 result['restored']={}
 for host in reversed(changed):
  try:
   remote(host,probe,[binary,sha,criu_mode]);remote(host,switch,[binary,previous[host]['installed']]);result['restored'][host]=True
  except Exception as e:result['restored'][host]=str(e);result['success']=False
 (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
 print('RESULT='+str(out/'result.json'),flush=True)
if not result['success']:raise SystemExit(1)
print('SWIFTBATON_BASELINE_PASS baseline='+a.baseline)
