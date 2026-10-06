#!/usr/bin/env python3
"""Plot measured CPU-limit trials, with no smoke/paper-scale mixing."""
import argparse
import csv
import json
import math
import os
import sys
from collections import defaultdict
from pathlib import Path
from statistics import median

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'experiments'))
from plot_all import load_trial


def read_trial(path):
    t = load_trial(path)
    if not t or not t.get('cpu_validation_ok') or not t.get('application_cpu_validation_ok'): return None
    gaps = t['gaps'].get('cutover_window_gaps', [])
    if not gaps: raise ValueError('No measured cutover completion gap')
    gap = max(gaps, key=lambda g: g['duration_ms'])
    unique = []
    with (t['raw'] / 'throughput-10ms.csv').open() as f:
        for row in csv.DictReader(f):
            if not unique: first_wall = float(row['unix_seconds'])
            item = (float(row['elapsed_seconds']), int(row['completed_total']))
            if unique and (item[0] < unique[-1][0] or item[1] < unique[-1][1]):
                raise ValueError('Client completion counter moved backwards')
            if unique and item[0] == unique[-1][0]: unique[-1] = item
            else: unique.append(item)
    elapsed = np.array([x[0] for x in unique])
    counts = np.array([x[1] for x in unique])
    intervals, delta = np.diff(elapsed), np.diff(counts)
    valid = (intervals > 0) & (intervals <= .05)
    rates = np.full(len(intervals), np.nan)
    rates[valid] = delta[valid] / intervals[valid]
    bins = {}
    for end, span, count, good in zip(elapsed[1:], intervals, delta, valid):
        slot = bins.setdefault(int(end / .1), [end, 0., 0, True])
        slot[0] = end; slot[1] += span; slot[2] += int(count)
        slot[3] &= bool(good)
    smooth = list(bins.values())
    origin = gap['approximate_start_wall_ns'] / 1e9 - first_wall
    recovery = t['recovery']
    final = recovery['references']['target_final_10s']['mean_ops_per_second']
    hit = recovery['recovery']['target_final_10s']['0.1']['0.9'] if recovery['ttr_valid'] else None
    ttr = hit['start_after_service_seconds'] + recovery['service_anchor_elapsed_seconds'] - origin if hit else None
    summary = dict(mode=t['mode'], variant=t['variant'], trial=t['trial'], manifest=str(path),
                   raw=str(t['raw']), source_revision=t.get('source_revision'),
                   binary_sha256=t['binary_sha256'], parameters=t['state']['parameters'],
                   downtime_ms=gap['duration_ms'],
                   source_ops_s=recovery['references']['source_pre_checkpoint']['mean_ops_per_second'],
                   destination_ops_s=final, ttr90_s=ttr, destination_reference_valid=recovery['ttr_valid'],
                   full_migration_seconds=t['metrics']['full_migration_seconds'],
                   cpu_validation_ok=t['cpu_validation_ok'],
                   cpu_hosts={host: {'cpus': row['cpus'],
                                    'startup_count': len(row['result']['startup_audit']),
                                    'max_threads': row['result']['maximum_concurrent_threads'],
                                    'violations': row['result']['violations']}
                              for host, row in t['cpu_restriction'].items()})
    for phase in ('source-before-workload', 'target-after-workload'):
        p = t['raw'] / ('runtime-' + phase + '.json')
        if p.exists():
            runtime = json.loads(p.read_text())
            summary.setdefault('redis_runtime', {})[phase] = {
                'host_config': runtime['host_config'],
                'task_affinity': {tid: row['affinity'] for tid, row in runtime['tasks'].items()}}
    return dict(summary=summary, origin=origin, time=elapsed[1:], rate=rates,
                t100=np.array([s[0] for s in smooth]),
                r100=np.array([s[2] / s[1] if s[3] and s[1] > 0 else np.nan for s in smooth]),
                checkpoint=t['metrics']['events_elapsed_seconds']['checkpoint_start'])


def label(key):
    variant = key[1]
    count = variant[:-4] if variant.endswith('core') else None
    suffix = 'default CPUs' if variant == 'unrestricted' else (count + ' cores' if count and count.isdigit() else variant)
    return 'SwiftBaton-' + key[0].upper() + ' (' + suffix + ')'


def style(key):
    return {'unrestricted': '-', '2core': '--', '4core': '-.', '8core': ':'}.get(key[1], '-')


def key_order(key):
    variant = key[1]
    count = variant[:-4] if variant.endswith('core') else ''
    rank = 0 if variant == 'unrestricted' else int(count) if count.isdigit() else float('inf')
    return (['u', 'k'].index(key[0]), rank, variant)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--results', type=Path, default=Path(os.environ.get(
        'SB_BENCH_RESULTS', ROOT.parent / (ROOT.name + '-benchmark-results'))))
    p.add_argument('--output', type=Path)
    p.add_argument('--extra-results', type=Path, action='append', default=[],
                   help='Merge another result root; repeat to include several')
    p.add_argument('--smoke', action='store_true')
    a = p.parse_args()
    out = a.output or a.results / 'plots' / ('cpu-limit-smoke' if a.smoke else 'cpu-limit')
    out.mkdir(parents=True, exist_ok=True)
    groups, skipped = defaultdict(list), []
    roots = [a.results] + a.extra_results
    paths = sorted({path.resolve() for root in roots
                    for path in (root / 'robustness-cpu-limit').glob('*/experiment.json')})
    for path in paths:
        info = json.loads(path.read_text())
        if bool(info.get('smoke')) != a.smoke: continue
        try: t = read_trial(path)
        except (KeyError, ValueError, OSError) as e:
            skipped.append({'manifest': str(path), 'reason': str(e)}); continue
        if t: groups[(t['summary']['mode'], t['summary']['variant'])].append(t)
        else: skipped.append({'manifest': str(path), 'reason': 'Failed, incomplete, or CPU verification failed'})
    if not groups:
        print(json.dumps({'skipped': skipped}, indent=2))
        raise SystemExit('No validated CPU-limit trials')
    keys = sorted(groups, key=key_order)
    for mode in {key[0] for key in keys}:
        trials = [t['summary'] for key in keys if key[0] == mode for t in groups[key]]
        if len({t['binary_sha256'] for t in trials}) != 1:
            raise SystemExit('Refusing to combine different CRIU binaries for mode ' + mode)
        if len({json.dumps(t['parameters'], sort_keys=True) for t in trials}) != 1:
            raise SystemExit('Refusing to combine different workload/migration parameters for mode ' + mode)
    chosen = {key: sorted(groups[key], key=lambda t: t['summary']['downtime_ms'])[len(groups[key]) // 2]
              for key in keys}
    colors = {'u': '#2166ac', 'k': '#d66028'}
    plt.rcParams.update({'font.size': 10, 'axes.spines.top': False, 'axes.spines.right': False})
    fig, axes = plt.subplots(2, 1, figsize=(11, 8), constrained_layout=True)
    for key in keys:
        selected = chosen[key]
        for t in groups[key]:
            axes[0].plot(t['t100'] - t['origin'], t['r100'] / 1000,
                         color=colors[key[0]], ls=style(key),
                         alpha=.15, lw=.7)
        x = selected['t100'] - selected['origin']
        axes[0].plot(x, selected['r100'] / 1000, color=colors[key[0]],
                     ls=style(key), lw=1.5, label=label(key))
        reference = selected['summary']['destination_ops_s']
        axes[1].plot(x, selected['r100'] / reference, color=colors[key[0]],
                     ls=style(key), lw=1.5, label=label(key))
    recovery_window = max(12, max(t['summary']['downtime_ms'] / 1000 + 5
                                  for items in groups.values() for t in items))
    for ax in axes:
        ax.set_xlim(-1, recovery_window); ax.set_ylim(bottom=0); ax.grid(axis='y', alpha=.2)
        ax.axvline(0, color='#555555', ls=':', lw=.8)
        ax.set_xlabel('Time from measured client completion-gap start (s)')
        ax.legend(fontsize=9, ncol=2 if len(keys) <= 4 else 4)
    axes[0].set_ylabel('Throughput (thousand ops/s)')
    axes[1].set_ylabel('Throughput / final destination mean')
    axes[1].axhline(.9, color='#888888', ls=':', lw=.8)
    parameters = chosen[keys[0]]['summary']['parameters']
    fig.suptitle('Redis migration with shared CRIU CPU affinity\n'
                 f"{parameters['records']:,} keys x {parameters['field_length']/1024:g} KiB; YCSB-A; "
                 f"Zipf {parameters['zipf_zeta']}; {parameters['threads']} YCSB threads; RDMA 25 Gbps"
                 + (' [SMOKE DATASET]' if a.smoke else ''))
    fig.savefig(out / 'cpu-limit-recovery.png', dpi=180)
    fig.savefig(out / 'cpu-limit-recovery.pdf'); plt.close(fig)
    rows = math.ceil(len(keys) / 2)
    fig, axes = plt.subplots(rows, 2, figsize=(12, 3.5 * rows), squeeze=False, constrained_layout=True)
    for ax in list(axes.flat)[len(keys):]: ax.set_visible(False)
    for ax, key in zip(axes.flat, keys):
        t = chosen[key]; s = t['summary']
        ax.plot(t['t100'], t['r100'] / 1000, color=colors[key[0]], lw=1)
        ax.axvline(t['checkpoint'], color='#777777', ls='--', lw=.8, label='Checkpoint/PS starts')
        ax.axvspan(t['origin'], t['origin'] + s['downtime_ms'] / 1000,
                   color='#b2182b', alpha=.3, label='Client interruption')
        ax.set(xlabel='Time since client sampling began (s)', ylabel='Thousand ops/s',
               title=label(key), ylim=(0, None))
        ax.grid(axis='y', alpha=.2); ax.legend(fontsize=8)
    fig.savefig(out / 'cpu-limit-full-workload.png', dpi=180)
    fig.savefig(out / 'cpu-limit-full-workload.pdf'); plt.close(fig)
    fig, axes = plt.subplots(rows, 2, figsize=(12, 3.5 * rows), squeeze=False, constrained_layout=True)
    for ax in list(axes.flat)[len(keys):]: ax.set_visible(False)
    for ax, key in zip(axes.flat, keys):
        t = chosen[key]; s = t['summary']
        ax.step((t['time'] - t['origin']) * 1000, t['rate'] / 1000,
                where='pre', color=colors[key[0]], lw=1)
        ax.axvspan(0, s['downtime_ms'], color='#b2182b', alpha=.15)
        ax.set(xlabel='Time from completion-gap start (ms)', ylabel='Thousand ops/s',
               title=f"{label(key)}: {s['downtime_ms']:.2f} ms", ylim=(0, None),
               xlim=(-200, max(300, s['downtime_ms'] + 150)))
        ax.grid(axis='y', alpha=.2)
    fig.savefig(out / 'cpu-limit-downtime.png', dpi=180)
    fig.savefig(out / 'cpu-limit-downtime.pdf'); plt.close(fig)
    summary = {'definitions': {
        'curve': 'Completed-operation counter differences, 100 ms aggregation; gaps in sampling over 50 ms remain missing.',
        'selection': 'All trials shown faintly; highlighted trial has the middle downtime in each group. This is a real trial, not an averaged synthetic curve.',
        'downtime': 'Largest all-client successful-operation gap in the cutover window, measured in client monotonic time.',
        'ttr90': '100 ms rolling throughput >=90% of the valid final destination mean, sustained 1 s; start time measured from completion-gap start.',
        'scope': 'CRIU and dedicated K dispatch/export workers share the selected CPUs on distinct physical cores per host. Redis, YCSB, application-context K fault callbacks and NIC IRQs are not capped.'},
        'result_roots': [str(root.resolve()) for root in roots],
        'groups': {}, 'runs': [], 'skipped': skipped}
    for key in keys:
        stats = {'count': len(groups[key]), 'selected_trial': chosen[key]['summary']['trial']}
        for metric in ('downtime_ms', 'ttr90_s', 'source_ops_s', 'destination_ops_s', 'full_migration_seconds'):
            values = [t['summary'][metric] for t in groups[key]]
            valid = [v for v in values if v is not None and math.isfinite(v)]
            stats[metric] = {'median': median(valid), 'min': min(valid), 'max': max(valid), 'n': len(valid)} if valid else None
        summary['groups'][label(key)] = stats
        summary['runs'].extend(t['summary'] for t in groups[key])
    (out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps({'output': str(out), 'groups': summary['groups'], 'skipped': skipped}, indent=2))


if __name__ == '__main__': main()
