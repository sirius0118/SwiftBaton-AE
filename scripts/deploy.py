#!/usr/bin/env python3
"""Stage this package on knode1/3, or activate its CRIU. Preview by default."""
import argparse,json,os,shlex,subprocess,sys
from pathlib import Path
from preflight import CONFIG,ROOT,inspect,digest,offline

def run(host,args):
    cmd=args if host=='knode2' else ['ssh','-oBatchMode=yes','-oConnectTimeout=8',host,shlex.join(args)]
    return subprocess.run(cmd,check=True,text=True,stdout=subprocess.PIPE).stdout

def idle(data):
    for host in ['knode2','knode3']:
        v=data[host]
        if v['active_criu']['rc']==0 or v['ae_containers']['rc'] or v['ae_containers']['text']:
            raise SystemExit(host+': running CRIU / retained AE containers / failed Docker query. Reserve the cluster and clean only your own previous run first.')

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('action',choices=['stage','image','activate','restore'])
    p.add_argument('--execute',action='store_true')
    a=p.parse_args()
    if a.action=='stage':
        commands=[['rsync','-az','--checksum','--exclude=.git/','--exclude=ae-work/',
                   '--exclude=results/','--exclude=/build/','--exclude=/.venv/',
                   '--exclude=__pycache__/','--exclude=.run.lock','--exclude=.DS_Store',
                   '--exclude=runtime-backup/',
                   '-e','ssh -oBatchMode=yes',str(ROOT)+'/',host+':'+CONFIG['root']+'/'] for host in ['knode1','knode3']]
        for cmd in commands:print(shlex.join(cmd),flush=True)
    elif a.action=='image':
        print(shlex.join(['docker','pull',CONFIG['redis_pull']]))
        print('docker save '+CONFIG['redis_image']+' | ssh knode3 docker load')
    elif a.action=='activate':
        print('Activate '+CONFIG['root']+'/criu/criu/criu through /usr/bin/criu on knode2 and knode3; save previous symlinks in runtime-backup/.')
    else:print('Restore the exact previous /usr/bin/criu symlinks from runtime-backup/.')
    if not a.execute:
        print('PREVIEW ONLY. Add --execute in a reserved experiment window.');return
    if str(ROOT)!=CONFIG['root']:raise SystemExit('Run on knode2 from '+CONFIG['root'])
    data=inspect();idle(data)
    if data['knode2']['hostname']!=CONFIG['hostnames']['knode2']:raise SystemExit('This command must run on knode2')
    if a.action=='image':
        subprocess.run(['docker','pull',CONFIG['redis_pull']],check=True)
        actual=run('knode2',['docker','image','inspect','--format','{{.Id}}',CONFIG['redis_pull']]).strip()
        if actual!=CONFIG['redis_image']:raise SystemExit('Downloaded image ID differs from configuration')
        sender=subprocess.Popen(['docker','save',CONFIG['redis_image']],stdout=subprocess.PIPE)
        try:
            receiver=subprocess.Popen(['ssh','-oBatchMode=yes','knode3','docker','load'],stdin=sender.stdout)
            sender.stdout.close()
            receiver_status=receiver.wait()
            sender_status=sender.wait()
            if sender_status or receiver_status:raise SystemExit('Image transfer failed')
        finally:
            if sender.poll() is None:sender.terminate();sender.wait()
        print('Image loaded on source and destination. No containers started.');return
    if a.action=='stage':
        errors=offline(built=True)
        if errors:raise SystemExit('Build first: '+str(errors))
        for host,cmd in zip(['knode1','knode3'],commands):
            guard="from pathlib import Path;p=Path("+repr(str(ROOT))+");assert not p.exists() or not any(p.iterdir()) or ((p/'configs/lab.json').is_file() and (p/'DualDriver/script/run_ae.py').is_file()),'Refusing to overwrite an unrelated directory';p.mkdir(parents=True,exist_ok=True)"
            run(host,['python3','-c',guard]);subprocess.run(cmd,check=True)
            print(host+': package copied; no services, runtime links, containers or network rules changed.')
        return
    target=str(ROOT/'criu/criu/criu')
    expected=digest(ROOT/'criu/criu/criu')
    # The shared-lab installation is an existing symlink. Never overwrite a
    # regular system binary. Refuse restore if another task changed the link.
    for host in ['knode2','knode3']:
        script='''import hashlib,json,os,pathlib
p=pathlib.Path('/usr/bin/criu');root=pathlib.Path(ROOT)
backup=root/'runtime-backup/criu-link.json'
assert p.is_symlink(), 'Administrator action required: /usr/bin/criu is not a symlink'
if ACTION=='activate':
 assert hashlib.sha256(pathlib.Path(TARGET).read_bytes()).hexdigest()==EXPECTED,'Package binary mismatch'
 if os.readlink(p)==TARGET:print('Already active');raise SystemExit(0)
 assert not backup.exists(),'Unrestored previous activation exists'
 backup.parent.mkdir(parents=True,exist_ok=True)
 backup.write_text(json.dumps({'previous':os.readlink(p),'activated':TARGET},indent=2)+'\\n')
 replacement=TARGET
else:
 saved=json.loads(backup.read_text())
 assert os.readlink(p)==saved['activated'],'Runtime link changed since activation; inspect manually'
 replacement=saved['previous']
temporary=p.with_name('criu.swiftbaton-ae-new')
assert not temporary.exists() and not temporary.is_symlink(),'Temporary link already exists'
temporary.symlink_to(replacement);os.replace(str(temporary),str(p))
if ACTION=='restore':backup.rename(backup.with_name('criu-link-restored.json'))
print('/usr/bin/criu -> '+replacement)
'''
        settings='ROOT='+repr(str(ROOT))+'\nTARGET='+repr(target)+'\nEXPECTED='+repr(expected)+'\nACTION='+repr(a.action)+'\n'
        print(host+': '+run(host,['sudo','-n','python3','-c',settings+script]).strip())
    print('No daemon was restarted. Run preflight.py --ready before any experiment.')
if __name__=='__main__':main()
