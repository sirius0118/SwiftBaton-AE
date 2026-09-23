#!/usr/bin/env python3
"""Remove only the experiment identified by a saved run_ae.py state file."""
import argparse
import json
import re
import shlex
import subprocess
import sys
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('state', type=Path)
parser.add_argument('--execute', action='store_true')
opts = parser.parse_args()
s = json.loads(opts.state.read_text())
if not opts.execute:
    print(json.dumps({'preview_only': True, 'run': s.get('name'), 'state': str(opts.state),
                      'action': 'remove only the experiment resources recorded in this state; preserve logs'}, indent=2))
    sys.exit(0)
root = Path(__file__).resolve().parents[2]
if opts.state.resolve().parent.parent != root / 'ae-work' or Path(s['out']).resolve() != opts.state.resolve().parent:
    raise SystemExit('Cleanup accepts only a current package run under '+str(root / 'ae-work'))
if not re.fullmatch(r'sb_ae_\d{8}_\d{6}', s.get('name', '')):
    raise SystemExit('Invalid experiment name')
name = s['name']
if not name.startswith('sb_ae_'):
    raise SystemExit('Not an AE experiment')

def run(host, argv, timeout=30):
    command = argv if host == 'knode2' else ['ssh', '-oBatchMode=yes', host, shlex.join(argv)]
    p = subprocess.run(command, text=True, capture_output=True, timeout=timeout)
    print(host, ' '.join(argv[:3]), 'exit=' + str(p.returncode), p.stdout[-1000:].strip(), p.stderr[-1000:].strip())
    return p

# Stop armed cutover helpers first: otherwise one could insert the NAT rule
# immediately after cleanup removed it.
for host, key in [('knode1', 'cutover_listener'), ('knode2', 'cutover_trigger')]:
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

if s.get('nat_rule'):
    rule = ['sudo', '-n', 'iptables', '-w', '10', '-t', 'nat', '-D', 'OUTPUT'] + s['nat_rule']
    result = run('knode1', rule)
    if result.returncode:
        check = run('knode1', ['sudo', '-n', 'iptables', '-w', '10', '-t', 'nat', '-C', 'OUTPUT'] + s['nat_rule'])
        if check.returncode != 1:
            raise SystemExit('Could not confirm the experiment NAT rule is absent; cleanup stopped')

for host, keys in [('knode1', ['load', 'run']), ('knode2', ['checkpoint']),
                   ('knode3', ['restore', 'pageclient'])]:
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

for host in ['knode2', 'knode3']:
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
    if host == 'knode3' and s.get('migration_dir') and not s.get('ram_images'):
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
