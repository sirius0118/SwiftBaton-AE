#!/usr/bin/env python3
"""Exact observed UFFD waits and locally timed per-page transport stages.

Never subtract clocks from different hosts. A transport sample identifies one
original (PID,address) transfer; forked contexts can share that transfer.
"""
import argparse,csv,json,math,re
from collections import defaultdict
from pathlib import Path
def summary(values):
 values=sorted(values)
 if not values:return {'count':0}
 def p(q):return values[max(0,math.ceil(q*len(values))-1)]/1000
 return dict(count=len(values),mean_us=sum(values)/len(values)/1000,min_us=values[0]/1000,
             p50_us=p(.5),p95_us=p(.95),p99_us=p(.99),p999_us=p(.999),max_us=values[-1]/1000)
def analyze(root):
 state=json.loads((root/'state.json').read_text());assert state['success']
 events={};drops={};modes={};gate=[];gate_policy=None;prefetch_window=None;fixed_ready_scan=None;ft_installer=None;pf_installer=None;fault_read_batch=None;source_observation=None
 for side,name in [('source','dump.log'),('target','pageclient.log')]:
  text=(root/name).read_text(errors='replace');group=defaultdict(lambda:defaultdict(list));total=0
  for line in text.splitlines():
   if 'SB_PF_TRACE ' in line:
    f={k:int(v) for k,v in re.findall(r'\b(stage|pid|address|ns)=(\d+)',line)}
    group[(f['pid'],f['address'])][f['stage']].append(f['ns']);total+=1
  headers=[{k:int(v) for k,v in re.findall(r'\b(count|dropped)=(\d+)',line)} for line in text.splitlines() if 'SB_PF_TRACE_SUMMARY ' in line]
  assert headers and sum(h['count'] for h in headers)==total
  drops[side]=sum(h['dropped'] for h in headers);events[side]=group
  matches=[{k:int(v) for k,v in re.findall(r'\b(synchronous|posted|completed|max_inflight)=(\d+)',line)} for line in text.splitlines() if 'SB_FAULT_TX ' in line]
  if side=='target' and not matches and state['parameters'].get('sync_fault_transport',False):
   modes[side]={'synchronous':1,'counter_status':'Synchronous target does not instantiate the asynchronous TX thread/counters'}
  else:
   assert len(matches)==1 and matches[0]['posted']==matches[0]['completed']
   modes[side]=matches[0]
  assert bool(modes[side]['synchronous'])==state['parameters'].get('sync_fault_transport',False)
  qp=[{k:int(v) for k,v in re.findall(r'\b(mtu|sl|tclass)=(\d+)',line)} for line in text.splitlines() if 'SB_RDMA_QP lane=demand ' in line]
  assert len(qp)==1 and qp[0]['sl']==7 and qp[0]['tclass']==224
  assert qp[0]['mtu']==state['parameters'].get('rdma_mtu',None) or qp[0]['mtu']==4096 and not state['parameters'].get('rdma_mtu')
  modes[side]['qp']=qp[0]
  if side=='source':
   m=re.search(r'SB_PF_OBSERVE observed=(\d+) accepted=(\d+) pending=(\d+) queue_retries=(\d+) tx_blocked_polls=(\d+)',text)
   if m:
    source_observation=dict(zip(['observed','accepted','pending','queue_retries','tx_blocked_polls'],map(int,m.groups())))
    assert source_observation['observed']==source_observation['accepted']+source_observation['pending']
    if not drops[side]:
     assert source_observation['observed']==sum(len(g.get(24,[])) for g in group.values())
     assert source_observation['accepted']==sum(len(g.get(0,[])) for g in group.values())
     for key,g in group.items():
      observed=sorted(g.get(24,[]));accepted=sorted(g.get(0,[]))
      assert len(observed)>=len(accepted) and all(o<=a for o,a in zip(observed,accepted)),(key,observed,accepted)
   m=re.search(r'SB_PREFETCH_WINDOW limit=(\d+) max_active=(\d+)',text)
   if m:
    limit,maximum=map(int,m.groups());prefetch_window=dict(limit=limit,max_active=maximum)
    assert limit==state['parameters']['prefetch_window'] and (not limit or maximum<=limit)
   m=re.search(r'SB_READY_SCAN fixed=(\d+)',text)
   if m:
    fixed_ready_scan=int(m[1]);assert bool(fixed_ready_scan)==state['parameters'].get('fixed_ready_scan',False)
  if side=='target':
   m=re.search(r'SB_FAULT_READ_BATCH configured=(\d+) effective=(\d+)',text)
   if m:
    configured,effective=map(int,m.groups());fault_read_batch=dict(configured=configured,effective=effective)
    assert configured==state['parameters']['fault_read_batch']
    assert effective==(configured if state['parameters']['fault_install_workers'] else 1)
   m=re.search(r'SB_DEMAND_INSTALL workers_per_pid=(\d+) processes=(\d+) dispatched=(\d+) installed=(\d+) acknowledged=(\d+) aux_queued=(\d+) aux_completed=(\d+) aux_fallback=(\d+)',text)
   if m:
    pf_installer=dict(zip(['workers_per_pid','processes','dispatched','installed','acknowledged','aux_queued','aux_completed','aux_fallback'],map(int,m.groups())))
    assert pf_installer['workers_per_pid']==state['parameters']['fault_install_workers']
    assert pf_installer['dispatched']==pf_installer['installed']==pf_installer['acknowledged']
    assert pf_installer['aux_queued']==pf_installer['aux_completed']
    if not drops[side]:
     assert pf_installer['installed']==sum(len(stages.get(8,[])) for stages in group.values())==sum(len(stages.get(23,[])) for stages in group.values())
   m=re.search(r'SB_PREFETCH_INSTALL serial=(\d+) workers=(\d+) dispatched=(\d+) installed=(\d+) acknowledged=(\d+)',text)
   if m:
    ft_installer=dict(zip(['serial','workers','dispatched','installed','acknowledged'],map(int,m.groups())))
    assert bool(ft_installer['serial'])==state['parameters'].get('serial_prefetch_install',False)
    assert ft_installer['dispatched']==ft_installer['installed']==ft_installer['acknowledged']
    if not drops[side]:assert ft_installer['installed']==sum(len(stages.get(16,[])) for stages in group.values())
   for line in text.splitlines():
    if 'SB_FAULT_GATE_POLICY ' in line:
     gate_policy=int(re.search(r'reader_preferred=(\d+)',line)[1])
    if 'SB_PID_FAULT_GATE ' in line:
     g={k:int(v) for k,v in re.findall(r'\b(pid|count|total_ns|max_ns)=(\d+)',line)}
     g['mean_us']=g['total_ns']/g['count']/1000 if g['count'] else None;gate.append(g)
   if gate_policy is not None:assert bool(gate_policy)==state['parameters'].get('reader_preferred_lock',False)
 samples=[];ambiguous=0
 for key,target in events['target'].items():
  if not target.get(8):continue
  source=events['source'].get(key,{})
  required_target=[5,6,7,8];required_source=[0,1,2,3,4]
  if any(not target.get(s) for s in required_target) or any(not source.get(s) for s in required_source):
   ambiguous+=1;continue
  if any(len(target[s])!=1 for s in [7,8]) or any(len(source[s])!=1 for s in [1,2,3,4]):
   ambiguous+=1;continue
  t={s:min(target[s]) for s in required_target};u={s:min(source[s]) for s in required_source}
  assert t[5]<=t[6]<=t[7]<=t[8] and u[0]<=u[1]<=u[2]<=u[3]<=u[4],(key,t,u)
  samples.append(dict(pid=key[0],address=key[1],target_queued_ns=t[5],target_installed_ns=t[8],
     target_queue_to_install_ns=t[8]-t[5],target_send_queue_ns=t[6]-t[5],
     target_post_to_response_ns=t[7]-t[6],target_install_ns=t[8]-t[7],
     source_queue_ns=u[1]-u[0],source_copy_wait_ns=u[2]-u[1],
     source_ready_to_post_ns=u[3]-u[2],source_service_ns=u[3]-u[0],source_post_to_cq_ns=u[4]-u[3],
     target_request_count=len(target[5]),source_request_count=len(source[0])))
  if source.get(24):
   first_observed=min(source[24]);assert first_observed<=u[0]
   samples[-1].update(source_observe_to_accept_ns=u[0]-first_observed,
       source_observe_to_post_ns=u[3]-first_observed)
  elif source_observation and not drops['source']:raise AssertionError('Missing source first observation')
  if target.get(23):
   assert len(target[23])==1 and t[6]<=target[23][0]<=t[7]
   samples[-1].update(target_dispatched_ns=target[23][0],
       target_post_to_dispatch_ns=target[23][0]-t[6],target_dispatch_to_receive_ns=t[7]-target[23][0],
       target_dispatch_to_install_ns=t[8]-target[23][0])
  elif pf_installer and not drops['target']:raise AssertionError('Missing target PF dispatch observation')
 keys=[k for k in (samples[0] if samples else {}) if k.endswith('_ns') and k not in ['target_queued_ns','target_installed_ns','target_dispatched_ns']]
 exact=[];unfinished=0
 with (root/'page-trace.csv').open() as f:
  for raw in csv.DictReader(f):
   row={k:int(v) for k,v in raw.items()}
   if not row['first_fault_read_ns']:continue
   if row['installed_ns']<row['first_fault_read_ns']:unfinished+=1;continue
   row['event_read_to_install_ns']=row['installed_ns']-row['first_fault_read_ns'];exact.append(row)
 ft_samples=[];ft_by_key={}
 for key,target in events['target'].items():
  source=events['source'].get(key,{})
  if any(len(source.get(s,[]))!=1 for s in range(10,15)) or any(len(target.get(s,[]))!=1 for s in [15,16]):continue
  u={s:source[s][0] for s in range(10,15)};t={s:target[s][0] for s in [15,16]}
  assert u[10]<=u[11]<=u[12]<=u[13]<=u[14] and t[15]<=t[16]
  row=dict(pid=key[0],address=key[1],source_claim_to_copy_ns=u[11]-u[10],source_copy_to_ready_ns=u[12]-u[11],
           source_ready_to_post_ns=u[13]-u[12],source_post_to_cq_ns=u[14]-u[13],
           source_claim_to_post_ns=u[13]-u[10],target_receive_to_install_ns=t[16]-t[15],
           target_received_ns=t[15],target_installed_ns=t[16])
  if source.get(0):row['source_post_after_demand_accept_ns']=u[13]-min(source[0])
  if target.get(17):
   assert len(target[17])==1 and target[17][0]<=t[15]
   row['target_dispatched_ns']=target[17][0]
   row['target_dispatch_to_receive_ns']=t[15]-target[17][0]
   row['target_dispatch_to_install_ns']=t[16]-target[17][0]
  elif ft_installer and not drops['target']:raise AssertionError('Missing target FT dispatch observation')
  ft_samples.append(row);ft_by_key[key]=row
 ft_waits=[]
 for wait in exact:
  if wait['lane']!=1:continue
  transfer=ft_by_key.get((wait['origin_pid'],wait['origin_address']))
  if transfer:
   ft_waits.append(dict(**wait,transfer=transfer,
    target_receive_after_event_ns=transfer['target_received_ns']-wait['first_fault_read_ns']))
 result=dict(run=root.name,binary=state['criu_sha256']['knode2'],transport=modes,
  event_gate_reader_preferred=gate_policy,event_gate_wait_by_pid=gate,
  prefetch_window=prefetch_window,fixed_ready_scan=fixed_ready_scan,source_observation=source_observation,
  prefetch_installer=ft_installer,demand_installer=pf_installer,fault_read_batch=fault_read_batch,
  dropped_events=drops,excluded_ambiguous_transfers=ambiguous,
  transfer_stages={k:summary([s[k] for s in samples]) for k in keys},
  transfer_stages_by_pid={str(pid):{k:summary([s[k] for s in samples if s['pid']==pid]) for k in keys} for pid in sorted({s['pid'] for s in samples})},
  uffd_waits_all_lanes=summary([r['event_read_to_install_ns'] for r in exact]),
  uffd_waits_by_install_lane={str(lane):summary([r['event_read_to_install_ns'] for r in exact if r['lane']==lane]) for lane in range(4)},
  uffd_waits_by_pid={str(pid):{str(lane):summary([r['event_read_to_install_ns'] for r in exact if r['lane']==lane and r['origin_pid']==pid]) for lane in range(4)} for pid in sorted({r['origin_pid'] for r in exact})},
  slowest_transfers=sorted(samples,key=lambda r:r['target_queue_to_install_ns'],reverse=True)[:20],
  slowest_observed_waits=sorted(exact,key=lambda r:r['event_read_to_install_ns'],reverse=True)[:20],
  prefetch_transfer_stages={k:summary([s[k] for s in ft_samples]) for k in (ft_samples[0] if ft_samples else {}) if k.endswith('_ns') and k not in ['target_dispatched_ns','target_received_ns','target_installed_ns','source_post_after_demand_accept_ns']},
  slowest_prefetch_resolved_faults=sorted(ft_waits,key=lambda r:r['event_read_to_install_ns'],reverse=True)[:20],
  retired_or_uninstalled_trace_pages=unfinished,
  definitions=dict(percentile='Exact nearest-rank percentile of retained observations, not a histogram bound.',
   uffd='First userspace event read to first installation for an original page/context. Excludes kernel-to-reader notification and application rescheduling; not complete kernel fault latency.',
   transport='One demand transfer per original PID/address. Earliest duplicate request used. Other lanes and PS hits are excluded from demand transport stages; their observed waits remain in lane statistics.',
   source_observation='Stage24 is the first userspace observation of the current request-ring entry, before TX-credit gating and scheduler admission; one event survives EAGAIN retries. Stage0 is after successful admission and may coalesce with another owner. Neither is NIC arrival. Delay before first observation, including descheduling and earlier ring entries, remains outside source_observe_to_post.',
   clocks='Every duration uses timestamps from one host only. Never subtract source and destination timestamps. target_post_to_response ends when an installer starts the received page, including dispatch queue time in the parallel target. The explicit post_to_dispatch and dispatch_to_receive fields separate them.',
   tracing='Opt-in prefaulted bounded buffers, timestamps on the path, log output after transfer threads join. Dropped-event counts are explicit. Compare modes with tracing equally enabled.'))
 (root/'fault-latency.json').write_text(json.dumps(result,indent=2)+'\n')
 with (root/'remote-fault-samples.csv').open('w') as f:
  w=csv.DictWriter(f,fieldnames=list(samples[0]) if samples else ['pid','address']);w.writeheader();w.writerows(samples)
 if ft_samples:
  with (root/'prefetch-transfer-samples.csv').open('w') as f:
   fields=list(dict.fromkeys(k for r in ft_samples for k in r))
   w=csv.DictWriter(f,fieldnames=fields);w.writeheader();w.writerows(ft_samples)
 print(root.name,json.dumps(result['transfer_stages'].get('target_queue_to_install_ns',{})), 'drops',drops)
 return result
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('directories',type=Path,nargs='+');a=p.parse_args()
 for d in a.directories:analyze(d)
