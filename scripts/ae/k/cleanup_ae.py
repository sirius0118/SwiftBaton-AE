#!/usr/bin/env python3
"""Remove only the experiment identified by a saved run_ae.py state file."""
import json
import shlex
import subprocess
import sys
from pathlib import Path

state_path = Path(sys.argv[1]).resolve()
s = json.loads(state_path.read_text())
if Path(s['out']).resolve() != state_path.parent or state_path.parent.name != s['name']:
    raise SystemExit('State ownership/path mismatch')
name = s['name']
if not name.startswith('sb_ae_'):
    raise SystemExit('Not an AE experiment')

def run(host, argv, timeout=30):
    command = argv if host == 'node2' else ['ssh', '-oBatchMode=yes', host, shlex.join(argv)]
    p = subprocess.run(command, text=True, capture_output=True, timeout=timeout)
    print(host, ' '.join(argv[:3]), 'exit=' + str(p.returncode), p.stdout[-1000:].strip(), p.stderr[-1000:].strip())
    return p

# Stop armed cutover helpers first: otherwise one could insert the NAT rule
# immediately after cleanup removed it.
for host, key in [('node1', 'cutover_listener'), ('node2', 'cutover_trigger'), ('node3', 'cutover_resume')]:
    pid = s.get(key + '_pid')
    if not pid:
        continue
    script = f'''import pathlib,os,signal
p=pathlib.Path('/proc/{pid}/cmdline')
if p.exists():
 a=p.read_bytes().replace(b'\\0',b' ').decode(errors='replace')
 if {name!r} in a and 'fast_cutover.py' in a:
  try:os.killpg({pid},signal.SIGKILL)
  except ProcessLookupError:pass
'''
    run(host, ['sudo', '-n', 'python3', '-c', script])

if s.get('parameters', {}).get('buffered_cutover'):
    result = run('node1', ['sudo', '-n', 'python3', str(Path(__file__).with_name('packet_gate.py')), 'cleanup', name])
    if result.returncode: raise SystemExit('Could not remove owned packet gate/NAT table')
    for host,side in [('node1','client'),('node3','target')]:
        args=['sudo','-n','python3',str(Path(__file__).with_name('nat_bindings.py')),'cleanup',str(state_path.parent/('cutover-nat-'+side+'.json')),name,side,str(s['port'])]
        if run(host,args).returncode:raise SystemExit('Could not remove exact experiment NAT bindings')


if s.get('nat_rule'):
    rule = ['sudo', '-n', 'iptables', '-w', '10', '-t', 'nat', '-D', 'OUTPUT'] + s['nat_rule']
    result = run('node1', rule)
    if result.returncode:
        check = run('node1', ['sudo', '-n', 'iptables', '-w', '10', '-t', 'nat', '-C', 'OUTPUT'] + s['nat_rule'])
        if check.returncode != 1:
            raise SystemExit('Could not confirm the experiment NAT rule is absent; cleanup stopped')

if s.get('transfer_mode') == 'K' and s.get('node3_cid'):
    # An incomplete K destination can still need source MRs. Destroy that exact
    # owned container before terminating source controllers and dropping pins.
    cid = s['node3_cid']
    inspected = run('node3', ['docker', 'inspect', cid])
    if inspected.returncode == 0:
        info = json.loads(inspected.stdout)[0]
        if info.get('Id') != cid or info.get('Config', {}).get('Labels', {}).get('swiftbaton.ae') != 'true':
            raise SystemExit('K target identity mismatch; cleanup stopped')
        removed = run('node3', ['docker', 'rm', '-f', cid])
        if removed.returncode:
            raise SystemExit('Could not destroy K destination before source cleanup')
    elif 'No such' not in inspected.stderr:
        raise SystemExit('Could not inspect K destination; cleanup stopped')

for host, keys in [('node1', ['load', 'run', 'poststeady']), ('node2', ['checkpoint']),
                   ('node3', ['restore', 'pageclient'])]:
    for key in keys:
        pid = s.get(key + '_pid')
        if not pid:
            continue
        script = f'''import pathlib,os,signal,time
p=pathlib.Path('/proc/{pid}/cmdline')
if p.exists():
 a=p.read_bytes().replace(b'\\0',b' ').decode(errors='replace')
 if {name!r} in a or {s.get('migration_dir', '__not_set__')!r} in a:
  os.killpg({pid},signal.SIGTERM)
  time.sleep(.3)
  try:os.killpg({pid},signal.SIGKILL)
  except ProcessLookupError:pass
'''
        run(host, ['sudo', '-n', 'python3', '-c', script])

for host in ['node2', 'node3']:
    cid = s.get(host + '_cid', '__not_set__')
    source_pid = s.get('source_pid', -1)
    # runc restore and its descendants are owned by containerd, not docker CLI.
    script = f'''import pathlib,os,signal
processes={{}}
for p in pathlib.Path('/proc').iterdir():
 if not p.name.isdigit():continue
 try:
  a=(p/'cmdline').read_bytes().replace(b'\\0',b' ').decode(errors='replace')
  st=(p/'stat').read_text().rsplit(')',1)[1].split()
  processes[int(p.name)]=(int(st[1]),a)
 except (OSError,ValueError):pass
roots=set()
anchors=set()
for pid,(parent,a) in processes.items():
 if a.startswith('criu: dump --rpc -t {source_pid} '):roots.add(pid)
 if a.startswith('runc ') and {cid!r} in a:roots.add(pid)
 words=a.split()
 if words and words[0].rsplit('/',1)[-1]=='containerd-shim-runc-v2' and '-id' in words:
  j=words.index('-id')
  if j+1<len(words) and words[j+1]=={cid!r}:anchors.add(pid)
while True:
 more={{pid for pid,(parent,a) in processes.items() if parent in roots or parent in anchors}}
 if more <= roots:break
 roots |= more
for pid in roots:
 try:os.kill(pid,signal.SIGKILL)
 except ProcessLookupError:pass
print('terminated experiment runtime pids',sorted(roots))
'''
    run(host, ['sudo', '-n', 'python3', '-c', script])
    if host == 'node3' and s.get('migration_dir') and not s.get('ram_images'):
        run(host, ['sudo', '-n', 'umount', s['migration_dir'] + '/imgs_dir'])
    label = run(host, ['docker', 'inspect', '-f', '{{index .Config.Labels "swiftbaton.ae"}}', name])
    if label.returncode == 0 and label.stdout.strip() == 'true':
        run(host, ['docker', 'rm', '-fv', name])
    if s.get('ram_images') == '/dev/shm/swiftbaton-images-' + str(s.get('source_pid')):
        script = f'''from pathlib import Path
import shutil
p=Path({s['ram_images']!r})
owner=p/'ae-owner'
if owner.exists() and owner.read_text()=={name!r}:
 shutil.rmtree(p)
 print('removed owned RAM images',p)
'''
        run(host, ['sudo', '-n', 'python3', '-c', script])
print('Experiment logs and checkpoint metadata retained:', s['out'])
