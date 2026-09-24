#!/usr/bin/env python3
"""Measure client throughput recovery from real cumulative YCSB observations."""
import argparse,bisect,csv,json,math,re
from pathlib import Path

def coalesce(times,counts):
    t=[];c=[];duplicates=0
    for stamp,count in zip(times,counts):
        if t and (stamp<t[-1] or count<c[-1]):raise ValueError('Nonmonotonic samples/counter')
        if t and stamp==t[-1]:
            c[-1]=count;duplicates+=1
        else:t.append(stamp);c.append(count)
    return t,c,duplicates

def rolling(times,counts,width,max_gap=.05):
    out=[];bad=[0]
    for i in range(1,len(times)):
        if times[i]<=times[i-1] or counts[i]<counts[i-1]:raise ValueError('Nonmonotonic samples/counter')
        bad.append(bad[-1]+(times[i]-times[i-1]>max_gap))
    for i,t in enumerate(times):
        j=bisect.bisect_right(times,t-width+1e-8)-1
        valid=j>=0 and i>j and bad[i]==bad[j]
        out.append((counts[i]-counts[j])/(t-times[j]) if valid else None)
    return out

def sustained(times,rates,anchor,threshold,hold=1.,max_gap=.05):
    begun=None;previous=None
    for t,r in zip(times,rates):
        if t<anchor:continue
        if r is None or r<threshold or (previous is not None and t-previous>max_gap):
            begun=None
        if r is not None and r>=threshold:
            if begun is None:begun=t
            if t-begun>=hold-1e-8:
                return {'start_after_service_seconds':begun-anchor,'confirmed_after_service_seconds':t-anchor}
        previous=t
    return None

def interval_summary(times,counts,start,end,reference=None):
    indices=[i for i in range(1,len(times)) if times[i-1]>=start and times[i]<=end]
    if not indices:return None
    elapsed=sum(times[i]-times[i-1] for i in indices)
    completed=sum(counts[i]-counts[i-1] for i in indices)
    result={'observed_seconds':elapsed,'completed_operations':completed,'mean_ops_per_second':completed/elapsed,
            'maximum_sample_gap_seconds':max(times[i]-times[i-1] for i in indices)}
    if reference is not None:
        result['positive_operation_deficit']=sum(max(0.,reference*(times[i]-times[i-1])-(counts[i]-counts[i-1])) for i in indices)
        result['net_operation_deficit']=max(0.,reference*elapsed-completed)
    return result

def target_reference(times,counts,complete,settle=1.,window=10.,minimum=5.):
    """Use only complete observed intervals after actual migration completion."""
    if not math.isfinite(complete):raise ValueError('Missing migration completion marker')
    start=max(complete+settle,times[-1]-window)
    ref=interval_summary(times,counts,start,times[-1])
    if ref is None or ref['observed_seconds']<minimum:
        raise ValueError('Need at least 5 s of destination observations after full migration and settling')
    if ref['maximum_sample_gap_seconds']>.05:
        raise ValueError('Destination reference contains missing observations >50 ms')
    if ref['mean_ops_per_second']<=0:raise ValueError('Destination reference has no completed operations')
    buckets=[]
    edge=start
    while edge+1.<=times[-1]+1e-8:
        item=interval_summary(times,counts,edge,edge+1.)
        if item and item['observed_seconds']>=.9:buckets.append(item['mean_ops_per_second'])
        edge+=1.
    if len(buckets)<4:raise ValueError('Insufficient destination stability windows')
    mean=sum(buckets)/len(buckets)
    cv=math.sqrt(sum((v-mean)**2 for v in buckets)/len(buckets))/mean
    split=len(buckets)//2
    drift=abs(sum(buckets[split:])/len(buckets[split:])-sum(buckets[:split])/split)/mean
    ref.update(window_elapsed_seconds=[start,times[-1]],
        migration_complete_elapsed_seconds=complete,settling_seconds=settle,
        stability_bucket_seconds=1.,stability_coefficient_of_variation=cv,
        stability_half_window_relative_drift=drift,stable=cv<=.1 and drift<=.1)
    return ref

def relapse_intervals(times,rates,confirmed,threshold,max_gap=.05):
    """Retain later below-threshold observations and uncertainty, not just first recovery."""
    intervals=[];begin=None;last=None;gaps=[]
    for t,r in zip(times,rates):
        if t<confirmed:continue
        if r is None or (last is not None and t-last>max_gap):
            if begin is not None:
                intervals.append({'start':begin,'end':last,'duration_seconds':last-begin,'censored':True})
                begin=None
            gaps.append(t)
        elif r<threshold:
            if begin is None:begin=t
        elif begin is not None:
            intervals.append({'start':begin,'end':t,'duration_seconds':t-begin,'censored':False})
            begin=None
        last=t
    if begin is not None:
        intervals.append({'start':begin,'end':last,'duration_seconds':last-begin,'censored':True})
    return {'below_threshold_intervals':intervals,'unknown_sample_times':gaps,
            'sustained_relapse_count':sum(x['duration_seconds']>=1. for x in intervals)}

def analyze(root):
    with (root/'throughput-10ms.csv').open() as f:rows=list(csv.DictReader(f))
    times=[float(r['elapsed_seconds']) for r in rows];counts=[int(r['completed_total']) for r in rows]
    times,counts,duplicates=coalesce(times,counts)
    metrics=json.loads((root/'metrics.json').read_text());state=json.loads((root/'state.json').read_text())
    if not state['success']:raise ValueError('Do not treat a failed migration as a recovery success')
    z0,z1=metrics['zero_sample_window_seconds']
    if not math.isfinite(z1):raise ValueError('No observed zero interval: service anchor requires another marker')
    i=next(i for i in range(1,len(times)) if times[i]>z1 and counts[i]>counts[i-1])
    anchor=times[i]
    # All recovery intervals use the YCSB client's own timestamps and counters.
    # The old analyzer's source checkpoint marker selects only the reference window.
    left,right=metrics['baseline_window_seconds']
    complete=metrics['events_elapsed_seconds'].get('source_retired',math.nan)
    references={'source_pre_checkpoint':interval_summary(times,counts,left,right),
                'target_final_10s':target_reference(times,counts,complete)}
    result={'run':root.name,'binary':state['criu_sha256']['knode2'],'parameters':state['parameters'],
        'service_anchor_elapsed_seconds':anchor,'anchor_definition':'End of first positive operation-count interval following the recorded migration zero run. Client-observed service, not exact Tasks resumed or first request completion.',
        'zero_run_elapsed_seconds':[z0,z1],'same_timestamp_samples_coalesced':duplicates,'references':references,'recovery':{},'first_windows':{},'relapses':{},
        'primary_reference':'target_final_10s','ttr_valid':references['target_final_10s']['stable'],
        'definitions':{'smoothing_seconds':[.1,.5],'sustain_seconds':1.,'threshold_fractions':[.5,.8,.9],
        'gap_limit_seconds':.05,'missing_data':'No missing intervals are filled. Same-millisecond samples retain the final cumulative operation count at that timestamp. Threshold windows containing gaps >50ms are invalid. Fixed-window operations use complete observed intervals only.',
        'purpose':'Application throughput recovery, not minimum AS duration. Final target reference is retrospective and can differ from source capacity. Single-run comparison cannot establish causal strategy benefit.'}}
    for label,ref in references.items():
        result['recovery'][label]={}
        result['relapses'][label]={}
        for width in [.1,.5]:
            rates=rolling(times,counts,width)
            result['recovery'][label][str(width)]={str(fraction):sustained(times,rates,anchor,ref['mean_ops_per_second']*fraction) for fraction in [.5,.8,.9]}
            result['relapses'][label][str(width)]={str(fraction):(None if value is None else
                relapse_intervals(times,rates,anchor+value['confirmed_after_service_seconds'],ref['mean_ops_per_second']*fraction))
                for fraction in [.5,.8,.9] for value in [result['recovery'][label][str(width)][str(fraction)]]}
        result['first_windows'][label]={str(seconds):interval_summary(times,counts,anchor,anchor+seconds,ref['mean_ops_per_second']) for seconds in [.1,.25,.5,1,3,5]}
    # Preserve the service-relative diagnostic while also reporting the paper's
    # downtime-start axis with the client's measured zero-run boundary.
    result['service_anchor_after_zero_run_start_seconds']=anchor-z0
    result['recovery_from_zero_run_start']={}
    for label,widths in result['recovery'].items():
        result['recovery_from_zero_run_start'][label]={}
        for width,fractions in widths.items():
            result['recovery_from_zero_run_start'][label][width]={fraction:(None if entry is None else {
                'start_seconds':entry['start_after_service_seconds']+anchor-z0,
                'confirmed_seconds':entry['confirmed_after_service_seconds']+anchor-z0})
                for fraction,entry in fractions.items()}
    result['definitions']['zero_run_axis']='Client sampled zero-run boundary, not exact freeze time. Source-reference results on this axis approximate the paper TTR origin; smoothing and 1 s sustained criterion remain explicit and may differ from the paper.'
    result['definitions']['primary_reference']='Destination only, after source_retired (a conservative full-migration completion marker) plus 1 s settling. Up to final 10 s, at least 5 s. One-second buckets must have CV and half-window relative drift <=10%; otherwise ttr_valid=false and computed values are diagnostic only. Source reference remains diagnostic.'
    result['definitions']['relapse']='All later below-threshold intervals are retained after first 1 s confirmed recovery; sustained relapse means >=1 s. Gaps are recorded as unknown, never recovery. Event/client clock alignment remains required; the service-relative axis uses only client timestamps.'
    log=(root/'run.log').read_text(errors='replace')
    reconnect=[(int(attempts),int(ns)) for attempts,ns in re.findall(r'重连(\d+)轮，耗时：(\d+) 纳秒',log)]
    result['client_reconnect']={'logged_successful_reconnect_calls':len(reconnect),
        'maximum_call_ms':max((ns/1e6 for _,ns in reconnect),default=None),
        'minimum_call_ms':min((ns/1e6 for _,ns in reconnect),default=None),
        'attempts_per_logged_call':[n for n,_ in reconnect],
        'note':'Existing binding prints only successful reconnect calls without thread IDs or timestamps. Counts do not prove distinct clients or TCP-state preservation. Application recovery may include reconnect/timeout effects.'}
    (root/'recovery-metrics.json').write_text(json.dumps(result,indent=2)+'\n')
    return result

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('directories',nargs='+',type=Path);p.add_argument('--output',type=Path)
    a=p.parse_args();results=[analyze(d) for d in a.directories]
    if a.output:a.output.write_text(json.dumps({'runs':results},indent=2)+'\n')
    for r in results:
        print(r['run'],'source_ref',round(r['references']['source_pre_checkpoint']['mean_ops_per_second']),
              'target_ref',round(r['references']['target_final_10s']['mean_ops_per_second']),
              'target_TTR90_100ms',r['recovery']['target_final_10s']['0.1']['0.9'],
              'target_TTR90_500ms',r['recovery']['target_final_10s']['0.5']['0.9'],
              'first_1s_ops',r['first_windows']['target_final_10s']['1']['completed_operations'])
