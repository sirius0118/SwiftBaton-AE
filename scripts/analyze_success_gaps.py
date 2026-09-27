#!/usr/bin/env python3
"""Intersect every YCSB worker's gaps in one JVM's monotonic clock domain."""
import argparse
import csv
import json
from pathlib import Path

def intersect(left, right):
    out=[];i=j=0
    while i<len(left) and j<len(right):
        start=max(left[i][0],right[j][0]);end=min(left[i][1],right[j][1])
        if start<end:out.append((start,end))
        if left[i][1]<right[j][1]:i+=1
        else:j+=1
    return out

def analyze(directory, threads, cutover=None):
    paths=sorted(Path(directory).glob('worker-*.csv'))
    if len(paths)!=threads:raise ValueError(f'Expected {threads} workers, found {len(paths)}')
    all_gaps=None;anchors=[];total=0;thresholds=set()
    for path in paths:
        with path.open() as f:
            header=dict((k,int(v)) for k,v in (x.split('=') for x in next(f)[2:].split()))
            rows=[(int(r['start_ns']),int(r['end_ns'])) for r in csv.DictReader(f)]
        if header['dropped'] or header['successes']<2 or len(rows)!=header['count']:
            raise ValueError('Incomplete completion journal: '+str(path))
        previous=header['first']
        for a,b in rows:
            if not previous<=a<b<=header['last'] or b-a<header['threshold_ns']:
                raise ValueError('Invalid completion interval: '+str(path))
            previous=b
        total+=header['successes'];thresholds.add(header['threshold_ns'])
        anchors.append(header['anchor_wall_ms']*1000000-(header['anchor_before']+header['anchor_after'])//2)
        all_gaps=rows if all_gaps is None else intersect(all_gaps,rows)
    if len(thresholds)!=1:raise ValueError('Mixed journal thresholds')
    threshold=thresholds.pop();offset=sorted(anchors)[len(anchors)//2]
    intervals=[dict(start_mono_ns=a,end_mono_ns=b,duration_ms=(b-a)/1e6,
                    approximate_start_wall_ns=a+offset,approximate_end_wall_ns=b+offset)
               for a,b in all_gaps if b-a>=threshold]
    selected=[]
    if cutover:
        gate=json.loads(Path(cutover).read_text())
        a=gate['activate_begin_ns'];b=gate['release_done_ns']
        selected=[x for x in intervals if x['approximate_start_wall_ns']<b+1000000000
                  and x['approximate_end_wall_ns']>a-1000000000]
    return dict(workers=len(paths),successful_operations=total,threshold_ms=threshold/1e6,
                all_global_gaps=intervals,cutover_window_gaps=selected,
                max_cutover_gap_ms=max((x['duration_ms'] for x in selected),default=None),
                wall_anchor_spread_ms=(max(anchors)-min(anchors))/1e6,
                definition='Intersection of intervals between consecutive successful DB returns in all client workers. Duration uses existing JVM monotonic timestamps; wall anchors only locate migration and have millisecond resolution. Intervals below the recording threshold are omitted. No worker is treated as idle when its journal is missing or overflowed.')

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('run',type=Path);a=p.parse_args()
    state=json.loads((a.run/'state.json').read_text())
    result=analyze(a.run/'success-gaps',state['parameters']['threads'],a.run/'cutover-client.json' if state['parameters'].get('buffered_cutover') else None)
    (a.run/'success-gap-metrics.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))
