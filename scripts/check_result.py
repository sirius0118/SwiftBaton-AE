#!/usr/bin/env python3
"""Offline validation of a completed run's recorded results. Does not contact hosts."""
import argparse,json,sys
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('state',type=Path);a=p.parse_args()
s=json.loads(a.state.read_text());root=a.state.parent;fail=[]
for key in ['success','source_retired']:
    if s.get(key) is not True:fail.append(key+' is not true')
v=json.loads((root/'validation.json').read_text())
if v.get('missing_or_wrong_length')!=0:fail.append('record validation failed')
params=s.get('parameters',{})
if v.get('checked_records')!=params.get('records') or v.get('sentinel_verified') is not True:
    fail.append('incomplete record scan or sentinel mismatch')
if params.get('image_rdma'):
    path=root/'image-verification.json'
    if not path.exists():fail.append('image verification missing: run verify_images.py BEFORE cleanup')
    else:
        i=json.loads(path.read_text())
        if i.get('equal') is not True or i.get('files_checked',0)<=0:fail.append('input images differ or zero images checked')
if params.get('canary_mib'):
    c=json.loads((root/'canary-verify.json').read_text())
    if c.get('byte_verified') is not True or c.get('bytes')!=params['canary_mib']*1024*1024:
        fail.append('bytewise canary failed')
if params.get('memory_children'):
    rows=json.loads((root/'memory-children-target.json').read_text())
    if not rows or any(x.get('ok') is not True for x in rows):fail.append('memory-child verification failed')
print(json.dumps({'name':s['name'],'ok':not fail,'errors':fail,'run_directory':str(root),
                  'scope':'Checks recorded run evidence; does not re-run migration or prove all paper results.'},indent=2))
sys.exit(bool(fail))
