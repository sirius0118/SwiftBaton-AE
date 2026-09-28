#!/usr/bin/env python3
"""Print matched Redis/YCSB baseline results from validated run artifacts.

Pass result.json for U/K profiles and state.json for the native CRIU driver.
Rejects mixed workload sizes and runs without complete validation/cleanup.
"""
import argparse
import json
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('runs', nargs='+', type=Path)
a = p.parse_args()
rows = []
workload = None
for path in a.runs:
    report = json.loads(path.read_text())
    if path.name == 'result.json':
        if not report.get('success') or report.get('cleanup_rc') != 0 or \
                not all(report.get('restored', {}).get(host) is True
                        for host in ('knode2', 'knode3')):
            p.error('baseline result is not fully validated and cleaned: ' + str(path))
        state_path = Path(report['state'])
        label = report['baseline']
        transport = 'kernel RDMA' if label == 'remote-fork' else 'native ibverbs'
    else:
        state_path = path
        label = report.get('baseline')
        if label != 'native-criu-rsocket' or not report.get('success') or \
                not all(report.get('selection_rollback', {}).get(host) is True
                        for host in ('knode2', 'knode3')):
            p.error('native CRIU result is not fully validated and cleaned: ' + str(path))
        transport = 'rsocket relay'
    state = json.loads(state_path.read_text())
    if not state.get('success'):
        p.error('migration state failed: ' + str(state_path))
    parameters = state['parameters']
    if label == 'remote-fork' and parameters.get('kernel_rsocket_proxy'):
        validation_path = state_path.parent / 'rsocket-remote-fork-validation.json'
        if report.get('analysis', {}).get('verify_rsocket.py') != 0 or not validation_path.is_file():
            p.error('remote-fork rsocket payloads were not verified: ' + str(state_path))
        validation = json.loads(validation_path.read_text())
        if validation.get('transport') != 'rsocket images and proxy demand pages':
            p.error('invalid remote-fork transport verification: ' + str(validation_path))
        transport = 'rsocket images + proxy demand pages'
    current = tuple(parameters[key] for key in ('records', 'field_length', 'threads', 'duration'))
    if workload is None:
        workload = current
    elif workload != current:
        p.error('runs have different records, value length, threads or duration')
    root = state_path.parent
    if label == 'pclive':
        delta = json.loads((root / 'pclive-delta-validation.json').read_text())
        if delta.get('transport') == 'rsocket':
            transport = 'rsocket PS + native ibverbs AS'
    if label in ('pclive', 'postcopy', 'hybrid') and (root / 'rsocket-as-validation.json').exists():
        validation = json.loads((root / 'rsocket-as-validation.json').read_text())
        if validation.get('transport') != 'rsocket direct-write AS':
            p.error('invalid rsocket AS validation: ' + str(root))
        if (validation.get('ps_snapshot') and validation.get('image_phases') and
                all(phase.get('transport') == 'rsocket'
                    for phase in validation['image_phases'])):
            transport = 'rsocket images + PS + AS'
        else:
            transport = ('rsocket PS + AS + native ibverbs images' if label == 'pclive' else
                         'rsocket AS + native ibverbs images' if label == 'postcopy' else
                         'rsocket AS + native ibverbs PS/images')
    gaps = json.loads((root / 'success-gap-metrics.json').read_text())
    recovery = json.loads((root / 'recovery-metrics.json').read_text())
    events = [json.loads(line) for line in (root / 'events.jsonl').read_text().splitlines()]
    checkpoint = next(e['time_ns'] for e in events if e['event'] == 'checkpoint_start')
    sentinel = next(e['time_ns'] for e in events if e['event'] == 'sentinel_verified')
    candidates = [g for g in gaps['all_global_gaps']
                  if g['approximate_end_wall_ns'] >= checkpoint - 5_000_000_000 and
                     g['approximate_start_wall_ns'] <= sentinel + 5_000_000_000]
    if not candidates:
        p.error('no measured success gap in migration window: ' + str(root))
    gap = max(candidates, key=lambda item: item['duration_ms'])['duration_ms']
    reference = recovery['references']['target_final_10s']
    if not reference['stable']:
        p.error('target throughput reference is not stable: ' + str(root))
    ttr = recovery['recovery']['target_final_10s']['0.1']['0.9']
    rows.append((label, transport, gap, reference['mean_ops_per_second'],
                 ttr['start_after_service_seconds'] if ttr else None,
                 ttr['confirmed_after_service_seconds'] if ttr else None, root))

print('Workload: %d records x %d bytes, %d client threads, %d s' % workload)
print('| Baseline | Payload transport | Client success gap (ms) | Stable target ops/s | TTR90 start / confirmed (s) | Artifact |')
print('| --- | --- | ---: | ---: | ---: | --- |')
for label, transport, gap, ops, start, confirmed, root in rows:
    ttr = ('%.3f / %.3f' % (start, confirmed)) if start is not None else 'unavailable'
    print('| %s | %s | %.3f | %.0f | %s | `%s` |' %
          (label, transport, gap, ops, ttr, root))
print('\nThese are single end-to-end observations; each row records its verified payload transport.')
