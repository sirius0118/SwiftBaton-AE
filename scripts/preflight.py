#!/usr/bin/env python3
"""Read-only prerequisite inspection. Never launches a workload or migration."""
import argparse, hashlib, json, shlex, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CONFIG = json.loads((ROOT / 'configs/lab.json').read_text())
def digest(p):
    h=hashlib.sha256()
    with open(p,'rb') as f:
        for b in iter(lambda:f.read(1048576),b''):h.update(b)
    return h.hexdigest()

# All remote commands below inspect configuration/state only. No devices are
# written, no sockets are bound, and no CRIU check/dump/restore operation runs.
INSPECT = r'''
import hashlib,json,os,pathlib,shutil,socket,subprocess
def run(args):
 try:
  p=subprocess.run(args,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=12)
  return {'rc':p.returncode,'text':p.stdout.strip()}
 except Exception as e:return {'rc':-1,'text':str(e)}
def hashfile(p):
 try:
  h=hashlib.sha256()
  with open(p,'rb') as f:
   for b in iter(lambda:f.read(1048576),b''):h.update(b)
  return h.hexdigest()
 except OSError:return None
root=pathlib.Path(AE_ROOT)
programs=['python3','java','mvn','ssh','rsync','docker','dockerd','runc','containerd','criu','redis-cli','iptables','conntrack','mlnx_qos','ibv_devinfo','gcc','make']
r={'hostname':socket.gethostname(),'kernel':os.uname().release,'os_release':pathlib.Path('/etc/os-release').read_text(),
   'programs':{n:shutil.which(n) for n in programs},'sudo':run(['sudo','-n','true']),
   'cpu':run(['lscpu']),'memory':run(['free','-b']),'disk':run(['df','-B1',str(root if root.exists() else root.parent)]),
   'rdma':run(['ibv_devinfo','-d','mlx5_1']),
   'qos':run(['sudo','-n','mlnx_qos','-i','ens4f1','-a']),
   'docker':run(['docker','version','--format','{{json .Server}}']),
   'image':run(['docker','image','inspect','--format','{{.Id}}',AE_IMAGE]),
   'runtime_paths':{},'runtime_hashes':{},'active_criu':run(['pgrep','-x','criu']),
   'ae_containers':run(['docker','ps','-aq','--filter','label=swiftbaton.ae=true']),
   'listeners':run(['ss','-H','-ltn']),
   'collect_access':pathlib.Path('/dev/collect_access').exists(),
   'page_idle':pathlib.Path('/sys/kernel/mm/page_idle/bitmap').exists(),
   'bpf_tracepoints':run(['sudo','-n','test','-d','/sys/kernel/debug/tracing/events/syscalls/sys_enter_mmap'])['rc']==0,
   'java':run(['java','-version']),
   'package_binary':hashfile(root/'criu/criu/criu'),
   'fixture_binary':hashfile(root/'DualDriver/script/memory_fixture'),
   'ycsb_class':(root/'YCSB/redis/target/classes/site/ycsb/db/RedisClient.class').is_file(),
   'cutover_helper':(root/'DualDriver/script/fast_cutover.py').is_file()}
for name in ['criu','docker','dockerd','runc','containerd','containerd-shim-runc-v2']:
 p=shutil.which(name)
 if p:r['runtime_paths'][name]=os.path.realpath(p);r['runtime_hashes'][name]=hashfile(p)
print(json.dumps(r))
'''

def inspect():
    result={}
    for host in ['knode1','knode2','knode3']:
        cmd=['python3','-'] if host=='knode2' else ['ssh','-oBatchMode=yes','-oConnectTimeout=8',host,'python3','-']
        p=subprocess.run(cmd,input='AE_ROOT='+repr(CONFIG['root'])+'\nAE_IMAGE='+repr(CONFIG['redis_image'])+'\n'+INSPECT,text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=100)
        if p.returncode:raise RuntimeError(host+': '+p.stderr[-1500:])
        result[host]=json.loads(p.stdout)
    return result

def offline(built=False):
    errors=[]
    required=['criu/Makefile','criu/criu/sb-transfer.c','Fluid/agent/agent.py',
              'YCSB/pom.xml','DualDriver/script/run_ae.py','DualDriver/script/memory_fixture.c',
              'criu/module/collect-access.c','dependencies/runc/Makefile',
              'dependencies/docker-ce/components/engine/Makefile',
              'dependencies/docker-ce/components/engine/daemon/checkpoint.go',
              'dependencies/containerd/Makefile']
    if built:
        required += ['criu/criu/criu','DualDriver/script/memory_fixture',
                     'YCSB/redis/target/classes/site/ycsb/db/RedisClient.class',
                     'YCSB/core/target/classes/site/ycsb/Client.class']
        if not list((ROOT/'YCSB/redis/target/dependency').glob('jedis-*.jar')):
            errors.append('YCSB runtime dependencies missing; run scripts/build.sh ycsb')
    for name in required:
        if not (ROOT/name).is_file():errors.append('missing '+name)
    return errors

def readiness(data, deployed=True):
    errors=offline(built=True)
    if errors:return errors
    expected=digest(ROOT/'criu/criu/criu')
    for host in ['knode1','knode2','knode3']:
        v=data[host]
        if v['hostname']!=CONFIG['hostnames'][host]:errors.append(host+': unexpected hostname '+v['hostname'])
        if v['sudo']['rc']:errors.append(host+': passwordless sudo is required')
        for tool in ['python3','ssh','rsync']+(['java','redis-cli','iptables','conntrack'] if host=='knode1' else ['docker','criu','redis-cli','mlnx_qos','ibv_devinfo']):
            if not v['programs'].get(tool):errors.append(host+': missing '+tool)
        if deployed and not v['cutover_helper']:errors.append(host+': package not deployed')
        if host=='knode1':
            if v['java']['rc'] or '1.8.0' not in v['java']['text']:errors.append(host+': expected Java 8')
            if deployed and not v['ycsb_class']:errors.append(host+': packaged YCSB classes not deployed')
            continue
        if v['kernel']!=CONFIG['kernel']:errors.append(host+': kernel differs from frozen environment')
        if v['docker']['rc']:errors.append(host+': Docker inaccessible')
        else:
            try:
                d=json.loads(v['docker']['text'])
                if not d.get('Experimental'):errors.append(host+': Docker experimental mode disabled')
                # The custom runtime must be installed as documented in README.md.
            except ValueError:errors.append(host+': cannot parse Docker version')
        if v['runtime_hashes'].get('criu')!=expected:errors.append(host+': installed CRIU differs; activate only in a reserved idle window')
        if deployed and v['fixture_binary']!=digest(ROOT/'DualDriver/script/memory_fixture'):errors.append(host+': built memory fixture not deployed or differs')
        if deployed and v['package_binary']!=expected:errors.append(host+': packaged CRIU not deployed or has changed')
        if v['image']['rc'] or v['image']['text']!=CONFIG['redis_image']:errors.append(host+': pinned Redis image missing')
        if v['rdma']['rc'] or 'PORT_ACTIVE' not in v['rdma']['text']:errors.append(host+': RDMA port not ACTIVE')
        if v['qos']['rc']:errors.append(host+': cannot read '+CONFIG['network_interface']+' QoS')
        if not v['page_idle']:errors.append(host+': page-idle tracking missing')
        if host=='knode2' and not v['bpf_tracepoints']:errors.append(host+': source BPF syscall tracepoints unavailable')
        if v['active_criu']['rc']==0:errors.append(host+': CRIU processes already running')
        if v['ae_containers']['rc'] or v['ae_containers']['text']:errors.append(host+': retained AE containers or Docker query failure')
        for line in v['listeners']['text'].splitlines():
            fields=line.split()
            if len(fields)>3 and fields[3].rsplit(':',1)[-1] in ['6390','12346','4568']:errors.append(host+': required port already listening: '+fields[3])
    return errors

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--offline',action='store_true',help='check source files; no SSH')
    p.add_argument('--built',action='store_true',help='also require locally compiled CRIU, YCSB and fixture')
    p.add_argument('--inspect',action='store_true',help='print original host state without requiring deployment')
    p.add_argument('--ready',action='store_true',help='require deployed package and idle prerequisites')
    a=p.parse_args()
    if a.offline or not (a.inspect or a.ready):
        errors=offline(built=a.built);print(json.dumps({'mode':'offline','errors':errors,'ok':not errors},indent=2));return bool(errors)
    if ROOT!=Path(CONFIG['root']):raise SystemExit('The lab workflow must run from '+CONFIG['root'])
    data=inspect()
    if a.inspect:print(json.dumps(data,indent=2));return 0
    errors=readiness(data)
    print(json.dumps({'mode':'ready','errors':errors,'ok':not errors},indent=2))
    return bool(errors)
if __name__=='__main__':sys.exit(main())
