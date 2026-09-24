#!/usr/bin/env python3
"""Run on knode2 after an RDMA-image migration, before cleanup."""
import json
import shlex
import subprocess
import sys
from pathlib import Path

state = json.loads(Path(sys.argv[1]).read_text())
directory = state['ram_images']
assert directory == '/dev/shm/swiftbaton-images-' + str(state['source_pid'])
script = f'''import hashlib,json,pathlib,subprocess
p=pathlib.Path({directory!r})
files={{f.name:{{'bytes':f.stat().st_size,'sha256':hashlib.sha256(f.read_bytes()).hexdigest()}} for f in p.glob('*.img') if not f.name.startswith('stats-')}}
print(json.dumps({{'files':files,'filesystem':subprocess.check_output(['stat','-f','-c','%T',str(p)],text=True).strip()}}))
'''
result = {}
for host in ['knode2', 'knode3']:
    args = ['sudo', '-n', 'python3', '-c', script]
    if host != 'knode2':
        args = ['ssh', '-oBatchMode=yes', host, shlex.join(args)]
    result[host] = json.loads(subprocess.check_output(args, text=True))
result['equal'] = result['knode2']['files'] == result['knode3']['files']
result['files_checked'] = len(result['knode2']['files'])
result['note'] = 'Byte checksums for CRIU input images; excludes diagnostic stats images generated after transfer.'
print(json.dumps(result, indent=2))
assert result['equal'] and result['files_checked'] > 0
assert all(result[host]['filesystem'] == 'tmpfs' for host in ['knode2', 'knode3'])
