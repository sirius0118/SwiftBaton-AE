#!/usr/bin/env python3
"""Parse unmodified YCSB status lines; keep missing observations as missing."""
import argparse
import csv
import datetime as dt
import json
import math
import re
from pathlib import Path

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

parser = argparse.ArgumentParser()
parser.add_argument('directory', type=Path)
args = parser.parse_args()
root = args.directory
pattern = re.compile(r'^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d:\d{3}) .*? (\d+) operations;(?: ([\d.]+) current ops/sec;)?')
samples = []
for line in (root / 'run.log').read_text(errors='replace').splitlines():
    match = pattern.search(line)
    if not match:
        continue
    timestamp = dt.datetime.strptime(match[1], '%Y-%m-%d %H:%M:%S:%f').replace(
        tzinfo=dt.timezone(dt.timedelta(hours=8))).timestamp()
    samples.append((timestamp, int(match[2]), float(match[3]) if match[3] else math.nan))
if len(samples) < 10:
    raise SystemExit('Insufficient YCSB samples')
start = samples[0][0]
events = [json.loads(line) for line in (root / 'events.jsonl').read_text().splitlines()]
event_times = {e['event']: e['time_ns'] / 1e9 - start for e in events}
raw = []
for i, (timestamp, count, rate) in enumerate(samples):
    elapsed = timestamp - start
    interval = timestamp - samples[i-1][0] if i else 0
    completed = count - samples[i-1][1] if i else 0
    raw.append((timestamp, elapsed, count, rate, interval, completed))
with (root / 'throughput-10ms.csv').open('w') as f:
    writer = csv.writer(f)
    writer.writerow(['unix_seconds', 'elapsed_seconds', 'completed_total', 'reported_ops_per_sec',
                     'observed_interval_seconds', 'completed_since_previous_sample'])
    writer.writerows(raw)

# Aggregate actual operation deltas, weighted by the measured interval lengths.
bins = {}
for timestamp, elapsed, count, rate, interval, completed in raw[1:]:
    if interval <= 0:
        continue
    key = int(elapsed / .1)
    slot = bins.setdefault(key, [0, 0., elapsed])
    slot[0] += completed
    slot[1] += interval
    slot[2] = elapsed
aggregate = np.array([(value[2], value[0] / value[1]) for key, value in sorted(bins.items())])
with (root / 'throughput-100ms.csv').open('w') as f:
    writer = csv.writer(f)
    writer.writerow(['elapsed_seconds', 'observed_ops_per_sec'])
    writer.writerows(aggregate)

checkpoint = event_times['checkpoint_start']
cutover = event_times.get('network_cutover', math.nan)
zero_runs, active = [], []
for row in raw:
    if row[1] >= checkpoint and row[3] == 0:
        active.append(row[1])
    elif active:
        zero_runs.append(active)
        active = []
if active:
    zero_runs.append(active)
longest = max(zero_runs, key=lambda x: x[-1] - x[0]) if zero_runs else []
zero_start = longest[0] if longest else math.nan
zero_end = longest[-1] if longest else math.nan
before = aggregate[(aggregate[:, 0] >= checkpoint - 6) & (aggregate[:, 0] < checkpoint - 1), 1]
after = aggregate[aggregate[:, 0] >= aggregate[-1, 0] - 10, 1]
return_counts = {}
for op, status, count in re.findall(r'^\[([^,]+), Return=([^,]+), (\d+)',
                                   (root / 'run.log').read_text(errors='replace'), re.M):
    return_counts[op.rstrip(']') + ':' + status] = int(count)
metrics = dict(sample_count=len(raw), observed_duration_seconds=raw[-1][1],
               total_completed_operations=raw[-1][2],
               baseline_window_seconds=[checkpoint - 6, checkpoint - 1],
               baseline_mean_ops_per_second=float(before.mean()),
               last_10_seconds_mean_ops_per_second=float(after.mean()),
               zero_sample_window_seconds=[zero_start, zero_end],
               zero_sample_span_ms=(zero_end-zero_start)*1000,
               ycsb_return_counts=return_counts, events_elapsed_seconds=event_times,
               note='Zero span describes client sampling, not CRIU freeze time. Missing samples are not filled.')
(root / 'metrics.json').write_text(json.dumps(metrics, indent=2) + '\n')

plt.rcParams.update({'font.size': 10, 'axes.spines.top': False, 'axes.spines.right': False})
fig, axes = plt.subplots(2, 1, figsize=(12, 7.3), constrained_layout=True)
t = np.array([x[1] for x in raw]); rates = np.array([x[3] for x in raw])
for axis in axes:
    axis.plot(t, rates/1000, color='#a5c8df', linewidth=.5, alpha=.7, label='10 ms observations')
    axis.plot(aggregate[:, 0], aggregate[:, 1]/1000, color='#12678c', linewidth=1.35,
              label='100 ms aggregation')
    axis.axvline(checkpoint, color='#c17c00', linestyle='--', linewidth=1, label='Checkpoint command')
    if math.isfinite(cutover):
        axis.axvline(cutover, color='#bb3e55', linestyle='--', linewidth=1, label='Cutover acknowledged')
    if 'source_retired' in event_times:
        axis.axvline(event_times['source_retired'], color='#368159', linestyle='-.',
                     linewidth=1, label='Source Redis retired')
    axis.axhline(before.mean()/1000, color='#666666', linestyle=':', linewidth=.8)
    axis.set_ylabel('Throughput (thousand ops/s)')
    axis.set_xlabel('Time since YCSB sampling began (s)')
    axis.set_ylim(bottom=0)
    axis.grid(axis='y', alpha=.2)
axes[0].set_xlim(0, raw[-1][1])
axes[0].legend(loc='upper right', ncol=3, framealpha=.95)
axes[0].set_title('Redis live migration: knode2 → knode3, YCSB on knode1', loc='left', fontweight='bold')
if longest:
    axes[1].set_xlim(max(0, zero_start-2), min(raw[-1][1], zero_end+8))
    axes[1].axvspan(zero_start, zero_end, color='#bb3e55', alpha=.1)
axes[1].set_title('Migration interval — client throughput observations', loc='left')
state = json.loads((root / 'state.json').read_text())
params = state.get('parameters', {})
field_bytes = params.get('field_length', 1024)
field_label = f'{field_bytes / 1024:g} KiB' if field_bytes % 1024 == 0 else f'{field_bytes:,} B'
subtitle = f"{params.get('records', '?'):,} records × {field_label} · {params.get('threads', '?')} clients · workload A (50% reads, 50% updates)"
if params.get('memory_children'):
    subtitle += f"\n+ {params['memory_children']} child processes × {params['child_mib']} MiB × {params['child_workers']} threads, skewed memory access"
if params.get('cow_descendants'):
    subtitle += f"\nEach memory child also has one COW descendant ({params['memory_children'] * 2} memory processes total)"
if params.get('dynamic_memory'):
    subtitle += '\nEach child adds 16 MiB for discard, unmap/reuse, remap and fork during AS'
if params.get('fd_groups'):
    processes = params.get('memory_children', 0) * (2 if params.get('cow_descendants') else 1)
    subtitle += f"\n+ {processes * (15 * params['fd_groups'] + 2):,} fixture FDs: files, eventfds, epoll, pipes, TCP/UNIX queues, UDP and timers"
    coverage_path = root / 'fd-coverage.json'
    coverage = json.loads(coverage_path.read_text()) if coverage_path.exists() else {}
    adversarial = params.get('fd_adversarial', coverage.get('adversarial', False))
    subtitle += '\nSemaphore / preexisting UDP queue checks: ' + ('included' if adversarial else 'excluded (known baseline limitations)')
if 'failed' in event_times:
    subtitle = 'FAILED validation / checkpoint — throughput does not imply correctness\n' + subtitle
fig.set_size_inches(12, 7.3 + .2 * max(0, subtitle.count('\n') - 2))
fig.suptitle(subtitle, fontsize=10)
fig.savefig(root / 'throughput.png', dpi=180)
fig.savefig(root / 'throughput.pdf')
print(json.dumps(metrics, indent=2))
