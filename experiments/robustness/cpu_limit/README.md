# Redis migration with limited CRIU CPU resources

Run on Node 2 after the build, deployment and U/K preflight in the main
README. No manual workload commands are needed on Nodes 1 or 3. The runner
starts the YCSB client on Node 1 and migrates Redis from Node 2 to Node 3.

The full dataset is **1,000,000 keys with one 1,024-byte field**, Zipf 0.99,
YCSB-A (50% reads and 50% updates), 32 client threads, 180 seconds, and a
20-second warmup before checkpoint preparation. An additional 8 MiB
immutable canary checks migration integrity. All four groups use NUMA node 0
and a 25 Gbps hardware transmit cap on both migration hosts.

| Group | CPU control |
| --- | --- |
| SwiftBaton-U, unrestricted | Normal CRIU affinity |
| SwiftBaton-K, unrestricted | Normal CRIU affinity |
| SwiftBaton-U, 2core | All CRIU processes on each host share two CPUs |
| SwiftBaton-K, 2core | Same limit, also inherited by dedicated K export/dispatch workers |

The two CPUs are one hardware thread from each of two distinct physical
cores in the chosen NUMA node. Their numbers are discovered independently
on each host. This is a shared affinity mask for the entire collection of
CRIU processes and threads, not a two-CPU allowance for each process and
not a CPU-time quota. Worker counts stay unchanged so CPU availability is
the only changed setting within each mode. The normal U/K PS budgets and
worker configurations remain mode-specific; use the within-mode comparisons
to assess CPU sensitivity.

Redis container affinity and YCSB resources are unchanged across the four
groups. K fault callbacks execute in application context, and NIC interrupts
and shared kernel housekeeping are also outside the CRIU CPU limit. This
experiment does not restrict the complete host to two CPUs.

Rebuild **both** CRIU modes and stage them before the first run; older binaries
do not implement the startup control and are rejected by the runner's probe.
The runner never changes or reloads the kernel module.

```bash
cd "$HOME/SwiftBaton-AE"
bash scripts/build.sh U
bash scripts/build.sh K
python3 scripts/deploy.py --execute
python3 scripts/run.py U --check
python3 scripts/run.py K --check

experiments/robustness/cpu_limit/run.sh --dry-run
# Functionality check; these small runs are excluded from the full plots.
experiments/robustness/cpu_limit/run.sh --mode both --variant 2core --smoke --trials 1
# Four groups, three full trials each, with reversed ordering on even rounds.
experiments/robustness/cpu_limit/run.sh --mode both --trials 3
python3 experiments/plot_all.py
python3 experiments/robustness/cpu_limit/plot.py
```

`--mode u|k`, `--variant unrestricted|2core`, and `--trials N` select a subset.
`SB_BENCH_RESULTS` overrides the result root; otherwise results are stored
outside Git in the checkout's sibling `SwiftBaton-AE-benchmark-results/`.
The dedicated plotter accepts `--results PATH`, `--output PATH`, and
`--smoke`. It generates PNG/PDF recovery curves, complete workload curves,
client interruption detail, and `summary.json`. TTR90 uses a valid final
destination throughput reference, 100 ms windows and one second of sustained
recovery. It is measured from the observed client completion-gap start.
Missing sampling intervals are left missing. The highlighted curve is an
actual trial with the middle downtime in its group; other trials are shown
faintly. It is not a synthetic average curve.

The root-owned temporary control `/run/swiftbaton-ae/criu-cpus` is read at
CRIU startup before any worker is created, including Docker-launched RPC
workers. A startup audit covers short-lived CRIU executions. A 50 ms observer
checks thread affinities and dedicated K workers. CPU masks, physical-core
selection, thread observations, startup audits and control removal are saved
with each trial. A CPU validation failure marks that trial unsuccessful.
Successful trials also validate Redis state, stop the owned workload and
restore previous CRIU symlinks and bandwidth settings.

Do not overlap another CRIU operation with this experiment. The temporary
control applies to every newly started CRIU process on a migration host.
If a run is interrupted, inspect its logs and recover its owned migration
first. After CRIU has stopped, the saved CPU masks and `cpu-node2`/`cpu-node3`
directories identify the controls to remove with
`sudo python3 experiments/common/cpu_limit.py finish --cpus <SAVED_CPUS> --out <SAVED_OBSERVER_DIRECTORY>`
on each host. The helper refuses to remove a control while CRIU is running
or when its mask was changed by another owner.
