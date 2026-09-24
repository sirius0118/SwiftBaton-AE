#!/usr/bin/env python3
"""Check actual CRIU/RDMA lane accounting without synthesizing observations."""
import argparse,json,re
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('directory',type=Path)
a=p.parse_args();root=a.directory
source=(root/'dump.log').read_text(errors='replace')
target=(root/'pageclient.log').read_text(errors='replace')
state=json.loads((root/'state.json').read_text())
def record(text,marker):
    lines=[line for line in text.splitlines() if marker in line]
    if len(lines)!=1:raise ValueError(f'{marker}: expected one record, found {len(lines)}')
    line=lines[0]
    fields={k:int(v) for k,v in re.findall(r'([a-z_]+)=(\d+)',line)}
    stamp=re.search(r'^\(([\d.]+)\)',line)
    return fields,float(stamp[1]) if stamp else None
catalog,start=record(source,'SB_TRANSFER catalog ')
done,end=record(source,'SB_TRANSFER complete ')
installed,_=record(target,'SB_TRANSFER installed ')
assert state['parameters']['parallel_transfer']
assert state['criu_sha256']['knode2']==state['criu_sha256']['knode3']
assert done['pages']==done['committed']==catalog['pages']
assert done['precopy']==catalog['precopy_pending']
assert sum(done[x] for x in ('precopy','demand','prefetch','background'))==done['pages']
assert sum(installed[x] for x in ('demand','prefetch','background','existing')) + installed.get('discarded', 0)==done['pages']-done['precopy']
if installed['existing']==0 and installed.get('discarded',0)==0:
    assert all(done[x]==installed[x] for x in ('demand','prefetch','background'))
result={'source':done,'target':installed,'accounting_consistent':True,
        'all_three_lanes_exercised':all(done[x]>0 for x in ('demand','prefetch','background')),
        'catalog_to_final_ack_ms':(end-start)*1000 if end is not None and start is not None else None,
        'binary_sha256':state['criu_sha256']['knode2'],
        'note':'Counts are unique source ownership claims and target installation or lifecycle retirement outcomes. Fork fanout may install one received page in several descendant mappings; those copies are counted separately in lifecycle accounting. This is not per-fault latency, proof of hot-first speedup, or validation of dynamic VMAs or general multiprocess restore.'}
if 'SB_TRANSFER prefetch_pipeline ' in source:
    pipeline,_=record(source,'SB_TRANSFER prefetch_pipeline ')
    assert pipeline['pages']==done['prefetch']
    assert 0 < pipeline['completions'] <= pipeline['pages'] or pipeline['pages']==pipeline['completions']==0
    result['prefetch_pipeline']=pipeline
    result['prefetch_pages_per_completion']=pipeline['pages']/pipeline['completions'] if pipeline['completions'] else None
    helpers=[{k:int(v) for k,v in re.findall(r'([a-z_]+)=(\d+)',line)} for line in source.splitlines() if 'SB_TRANSFER fast_helpers ' in line]
    assert helpers
    assert all(h['fault_workers']==state['parameters']['fault_workers'] and h['prefetch_workers']==state['parameters']['prefetch_workers'] for h in helpers)
    result['fast_helpers']=helpers
if 'SB_TRANSFER fast_copies ' in source:
    copies=[{k:int(v) for k,v in re.findall(r'([a-z_]+)=(\d+)',line)} for line in source.splitlines() if 'SB_TRANSFER fast_copies ' in line]
    assert sum(r['pages'] for r in copies if r['lane']==0)==done['demand']
    assert sum(r['pages'] for r in copies if r['lane']==1)==done['prefetch']
    result['source_copy_workers']=copies
    result['multiple_workers_used']=any(r['active_workers']>1 for r in copies)
if 'SB_TRANSFER source_bg_profile ' in source:
    source_profile,_=record(source,'SB_TRANSFER source_bg_profile ')
    pipelined='SB_BG_PIPELINE ' in target
    target_profile,_=record(target,'SB_BG_PIPELINE ' if pipelined else 'SB_TRANSFER target_bg_profile ')
    assert source_profile['batches']==target_profile['batches']
    assert source_profile['batches']>0 or done['background']==0
    assert sum(source_profile[k] for k in ('metadata_ns','copy_wait_ns','publish_ns','credit_ns','final_ack_ns')) <= source_profile['total_ns']
    if pipelined:
        assert target_profile['pages']==done['background'] and target_profile['max_batches']<=9
        assert target_profile['publish_ns']+target_profile['ack_ns']<=target_profile['total_ns']
    else:assert sum(target_profile[k] for k in ('install_ns','ack_ns','idle_ns')) <= target_profile['total_ns']
    result['source_background_profile']=source_profile
    result['target_background_profile']=target_profile
    result['target_background_pipelined']=pipelined
    if 'copy_participants' in source_profile:
        assert source_profile['broadcast_participants']==source_profile['batches']*len(result['fast_helpers'])*state['parameters']['copy_workers']
        assert source_profile['batches'] <= source_profile['copy_participants'] <= min(source_profile['broadcast_participants'], done['background'])
        result['background_copy_ack_reduction_fraction']=1-source_profile['copy_participants']/source_profile['broadcast_participants']
    result['background_profile_note']='Each side uses its own monotonic clock. Components are wall times including scheduling and locks; source and target overlap and must not be added together. Source total excludes catalog setup; target total includes thread startup.'
if 'SB_BG_ASSIST ' in target:
    assist,_=record(target,'SB_BG_ASSIST ')
    assert bool(assist['serial'])==state['parameters'].get('serial_background_install',False)
    assert bool(assist['disabled'])==state['parameters'].get('no_bg_fault_assist',False)
    if not assist['serial']:assert assist['normal']+assist['direct']+assist['queued']==done['background']
    if assist['serial'] or assist['disabled']:assert assist['direct']==assist['queued']==assist['overflow']==0
    result['background_fault_assist']=assist
if 'SB_BG_ORDER ' in target:
    order,_=record(target,'SB_BG_ORDER ')
    assert bool(order['round_robin'])==state['parameters'].get('bg_round_robin',False)
    result['target_background_order']=order
elif 'bg_round_robin' in state['parameters'] and not state['parameters'].get('serial_background_install',False):
    raise AssertionError('Missing target background ordering verification')
(root/'transport-validation.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2))
