#!/usr/bin/env python3
"""Render every completed AE benchmark trial; never synthesize missing runs."""
import argparse
import csv
import json
import math
import os
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path
from statistics import median

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_RESULTS = Path(os.environ.get('SB_BENCH_RESULTS', ROOT.parent / (ROOT.name + '-benchmark-results')))
PHASE = re.compile(r'\bSB_PHASE phase=([^ ]+).*?\breal_ns=(\d+)')
U_FAULT = re.compile(r'\bSB_FAULT_SERVICE lane=2 count=(\d+) total_ns=(\d+)')
K_FAULT = re.compile(r'\bSB_KERNEL complete [^\n]*?fault_ns=(\d+)[^\n]*?faults=(\d+)')


def load_trial(manifest):
    info = json.loads(manifest.read_text())
    if not info.get('success') or info.get('cleanup_rc') or not info.get('criu_restored'):
        return None
    raw = Path(info['raw_result'])
    state = json.loads((raw / 'state.json').read_text())
    if not state.get('success'): return None
    metrics = json.loads((raw / 'metrics.json').read_text())
    recovery = json.loads((raw / 'recovery-metrics.json').read_text())
    with (raw / 'throughput-100ms.csv').open() as f:
        curve = [(float(r['elapsed_seconds']), float(r['observed_ops_per_sec'])) for r in csv.DictReader(f)]
    gaps = json.loads((raw / 'success-gap-metrics.json').read_text()) if (raw / 'success-gap-metrics.json').exists() else {}
    info.update(raw=raw, state=state, metrics=metrics, recovery=recovery, curve=curve, gaps=gaps)
    return info


def phase_times(raw):
    found = {}
    for name in ('dump.log', 'restore.log'):
        p = raw / name
        if not p.exists(): continue
        for label, value in PHASE.findall(p.read_text(errors='replace')):
            found.setdefault(label, []).append(int(value))
    return found


def phase_span(trial, begin, end):
    times = phase_times(trial['raw'])
    if begin not in times or end not in times: return None
    # Per-task phases can repeat; the first begin and final end enclose all tasks.
    return (max(times[end]) - min(times[begin])) / 1e6


def ttr(trial, fraction):
    recovery = trial['recovery']
    if not recovery.get('ttr_valid'): return None
    value = recovery['recovery']['target_final_10s']['0.1'].get(str(float(fraction)))
    if not value: return None
    # Keep the throughput reference and sustained-recovery rule identical to
    # the CPU comparison. Only the axis origin is shifted to the measured gap.
    origin = trial['metrics']['zero_sample_window_seconds'][0]
    gaps = trial['gaps'].get('cutover_window_gaps', [])
    if gaps:
        gap = max(gaps, key=lambda row: row['duration_ms'])
        with (trial['raw'] / 'throughput-10ms.csv').open() as f:
            first_wall = float(next(csv.DictReader(f))['unix_seconds'])
        origin = gap['approximate_start_wall_ns'] / 1e9 - first_wall
    return value['start_after_service_seconds'] + recovery['service_anchor_elapsed_seconds'] - origin


def downtime(trial):
    return trial['gaps'].get('max_cutover_gap_ms') or trial['metrics'].get('zero_sample_span_ms')


def fault_service(trial):
    path = trial['raw'] / 'pageclient.log'
    if not path.exists(): return None
    body = path.read_text(errors='replace')
    matches = K_FAULT.findall(body) if trial['mode'] == 'k' else U_FAULT.findall(body)
    if not matches: return None
    total, count = matches[-1] if trial['mode'] == 'k' else tuple(reversed(matches[-1]))
    count = int(count)
    return {'count': count, 'mean_us': int(total) / count / 1000} if count else None


def plot_curves(case, trials, output):
    fig, ax = plt.subplots(figsize=(9, 4.8), constrained_layout=True)
    groups = defaultdict(list)
    for t in trials: groups[(t['mode'], t['variant'])].append(t)
    for label, items in sorted(groups.items()):
        for index, item in enumerate(items):
            zero = item['metrics']['zero_sample_window_seconds'][0]
            if not math.isfinite(zero): continue
            base = item['metrics']['baseline_mean_ops_per_second']
            if not base or not math.isfinite(base): continue
            x = [a - zero for a, _ in item['curve']]
            y = [b / base for _, b in item['curve']]
            ax.plot(x, y, lw=1.1, alpha=.55, label=f'{label[0].upper()} {label[1]}' if index == 0 else None)
    ax.axvline(0, color='black', ls='--', lw=.8)
    ax.axhline(1, color='gray', ls=':', lw=.8)
    ax.set(xlim=(-2, 8), ylim=(0, None), xlabel='Seconds from observed zero-run start',
           ylabel='Throughput / pre-migration mean', title=case + ' — observed trials')
    ax.grid(axis='y', alpha=.2); ax.legend(fontsize=8, ncol=2)
    fig.savefig(output / 'throughput.png', dpi=180)
    fig.savefig(output / 'throughput.pdf')
    plt.close(fig)


def plot_metrics(case, trials, output):
    groups = defaultdict(list)
    for t in trials: groups[(t['mode'], t['variant'])].append(t)
    labels = [f'{mode.upper()}\n{variant}' for mode, variant in sorted(groups)]
    metrics = [
        ('Client downtime', downtime, 'ms'),
        ('TTR50', lambda x: ttr(x, .5), 's'),
        ('TTR100', lambda x: ttr(x, 1.), 's'),
        ('Pre-migration throughput',
         lambda x: x['metrics'].get('baseline_mean_ops_per_second'), 'ops/s'),
    ]
    fig, axes = plt.subplots(1, len(metrics), figsize=(max(12, len(labels) * .8 + 7), 4.6), constrained_layout=True)
    for ax, (title, func, unit) in zip(axes, metrics):
        shown = False
        for i, key in enumerate(sorted(groups)):
            values = [func(x) for x in groups[key]]
            values = [v for v in values if v is not None and math.isfinite(v)]
            if not values: continue
            shown = True
            middle = median(values)
            ax.bar(i, middle, color='#347ca1' if key[0] == 'k' else '#e6a24a')
            ax.errorbar(i, middle, yerr=[[middle - min(values)], [max(values) - middle]],
                        fmt='none', ecolor='black', capsize=3)
            ax.text(i, middle, str(len(values)), ha='center', va='bottom', fontsize=8)
        ax.set_xticks(range(len(labels)))
        ax.set_xticklabels(labels, rotation=35, ha='right', fontsize=8)
        ax.set(ylabel=unit, title=title); ax.grid(axis='y', alpha=.2)
        if not shown:
            ax.text(.5, .5, 'Threshold not reached' if title == 'TTR100' else 'No valid data',
                    transform=ax.transAxes, ha='center', va='center', fontsize=9, color='#666666')
    fig.suptitle(case + ' — median and observed range; n above bars')
    fig.savefig(output / 'metrics.png', dpi=180)
    fig.savefig(output / 'metrics.pdf')
    plt.close(fig)


def plot_breakdown(case, trials, output):
    spans = [('Network lock → dump images', 'dump.network_locked', 'dump.images_flushed'),
             ('Restore images → tasks resumed', 'restore.is_images_ready', 'restore.tasks_resumed'),
             ('FD reserve in PS', 'fd.reserve_ps_begin', 'fd.reserve_ps_done'),
             ('Kernel MR registration in PS', 'kernel.ps_register_begin', 'kernel.ps_register_done')]
    rows = []
    for t in trials:
        for label, begin, end in spans:
            value = phase_span(t, begin, end)
            if value is not None and value >= 0:
                rows.append((f'{t["mode"].upper()} {t["variant"]}', label, value))
    if not rows: return
    names = sorted(set(row[0] for row in rows))
    fig, ax = plt.subplots(figsize=(max(9, len(names) * .8 + 5), 5), constrained_layout=True)
    colors = ['#2b6f8e', '#d8864b', '#759f4c', '#9b72ad']
    width = .8 / len(spans)
    for j, (label, _, _) in enumerate(spans):
        for i, name in enumerate(names):
            values = [v for n, part, v in rows if n == name and part == label]
            if values:
                ax.bar(i - .4 + width * (j + .5), median(values), width,
                       color=colors[j], label=label if i == 0 else None)
    ax.set_xticks(range(len(names)))
    ax.set_xticklabels(names, rotation=35, ha='right')
    ax.set_ylabel('Elapsed time (ms)')
    ax.set_title(case + ' — instrumented phases (may overlap; do not sum)')
    ax.legend(fontsize=8); ax.grid(axis='y', alpha=.2)
    fig.savefig(output / 'phases.png', dpi=180)
    fig.savefig(output / 'phases.pdf')
    plt.close(fig)


def plot_faults(case, trials, output):
    groups = defaultdict(list)
    for t in trials:
        sample = fault_service(t)
        if sample: groups[(t['mode'], t['variant'])].append(sample['mean_us'])
    if not groups: return
    labels = [f'{mode.upper()}\n{variant}' for mode, variant in sorted(groups)]
    fig, ax = plt.subplots(figsize=(max(6, len(labels) * .8 + 3), 4.2), constrained_layout=True)
    for i, key in enumerate(sorted(groups)):
        values = groups[key]
        middle = median(values)
        ax.bar(i, middle, color='#347ca1' if key[0] == 'k' else '#e6a24a')
        ax.errorbar(i, middle, yerr=[[middle - min(values)], [max(values) - middle]],
                    fmt='none', ecolor='black', capsize=3)
        ax.text(i, middle, str(len(values)), ha='center', va='bottom', fontsize=8)
    ax.set_xticks(range(len(labels)))
    ax.set_xticklabels(labels, rotation=35, ha='right')
    ax.set(ylabel='Mean service time (µs)',
           title=case + ' — path-reported fault service time')
    ax.grid(axis='y', alpha=.2)
    fig.savefig(output / 'fault_service.png', dpi=180)
    fig.savefig(output / 'fault_service.pdf')
    plt.close(fig)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--results', type=Path, default=DEFAULT_RESULTS)
    p.add_argument('--output', type=Path)
    a = p.parse_args()
    output = a.output or a.results / 'plots'
    output.mkdir(parents=True, exist_ok=True)
    by_case = defaultdict(list)
    skipped = []
    for manifest in sorted(a.results.glob('*/**/experiment.json')):
        try: trial = load_trial(manifest)
        except (OSError, ValueError, KeyError) as exc:
            skipped.append({'manifest': str(manifest), 'reason': str(exc)}); continue
        if trial is None:
            skipped.append({'manifest': str(manifest), 'reason': 'incomplete or failed trial'}); continue
        # A 10K-record smoke test and a paper-scale run are different
        # experiments. Keeping them apart prevents an impressive-looking but
        # invalid median across incompatible working-set sizes.
        by_case[(trial['case'], bool(trial.get('smoke')))].append(trial)
    summary = {'cases': {}, 'skipped': skipped,
               'definitions': {'downtime': 'Client-wide successful-operation gap when available; otherwise 10 ms zero-run span',
                               'ttr': '100 ms rolling throughput, valid final destination reference, 1 s sustained threshold, measured completion-gap start (sampled zero-run start if no completion-gap trace)',
                               'plot': 'Only completed validated trials; no missing interval interpolation or simulated data'}}
    for (case, smoke), trials in sorted(by_case.items()):
        label = case + (' [smoke]' if smoke else ' [paper-scale]')
        path = output / (re.sub('[^a-z0-9_-]+', '-', case.lower()) +
                         ('-smoke' if smoke else '-paper-scale'))
        path.mkdir(exist_ok=True)
        plot_curves(label, trials, path)
        plot_metrics(label, trials, path)
        plot_breakdown(label, trials, path)
        plot_faults(label, trials, path)
        if case == 'robustness-cpu-limit':
            command = [sys.executable, str(ROOT / 'experiments/robustness/cpu_limit/plot.py'),
                       '--results', str(a.results), '--output', str(path)]
            if smoke: command.append('--smoke')
            subprocess.run(command, check=True)
        summary['cases'][label] = [{'variant': t['variant'], 'mode': t['mode'], 'trial': t['trial'],
                                    'smoke': smoke,
                                    'raw_result': str(t['raw']), 'downtime_ms': downtime(t),
                                    'ttr50_s': ttr(t, .5), 'ttr100_s': ttr(t, 1.),
                                    'pre_migration_ops_per_s': t['metrics'].get('baseline_mean_ops_per_second'),
                                    'fault_service': fault_service(t),
                                    'client_failure_fraction': t.get('client_result', {}).get('failure_fraction'),
                                    'poststeady_failure_fraction': t.get('poststeady_result', {}).get('failure_fraction')}
                                   for t in trials]
    (output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps({'output': str(output), 'cases': {k[0] + (' [smoke]' if k[1] else ' [paper-scale]'):len(v)
                                                          for k,v in by_case.items()},
                      'skipped': len(skipped)}, indent=2))


if __name__ == '__main__': main()
