# SwiftBaton AE experiments

Run these scripts **on knode2**. The runner coordinates knode1 (client), knode2
(source), and knode3 (destination), using the existing U/K migration drivers.
Every experiment directory has an executable `run.sh` and a reviewable
`case.json`. A case specifies the workload, its paper-scale parameters, and
independent variants. The runner does not generate measurement data.
This checkout expects the already deployed SwiftBaton U/K binaries and kernel
module on knode2/knode3. Database workloads build an isolated CRIU variant
from `/home/k8s/SwiftBaton-AE-k-target-preimport-20260928/{criu,criu-k}`;
that reviewed source tree must be present on knode2. The suite does not load
or replace the running kernel module.

## Quick start

```sh
cd /home/k8s/SwiftBaton-AE-rsocket-bridge-20260928
experiments/real_world/redis/run.sh --dry-run
experiments/real_world/redis/run.sh --mode u --smoke --trials 1
experiments/real_world/redis/run.sh --mode k --trials 5
experiments/robustness/zipf/run.sh --mode both --variant z0p99 --trials 5
python3 experiments/plot_all.py
```

Without `--smoke`, the values in `case.json` are used, and the default is five
trials of both U and K for every variant. `--smoke` uses 10,000 records, four
clients, 50 seconds of client runtime, and a 10-second warmup (128 MiB/four
workers for LargeContainer). It is a functionality test, not a paper-scale
result; variant-specific value size, Zipf skew, and write ratio are retained.
Use `--dry-run` to inspect all driver invocations before launching a
large matrix. Run one case at a time; `cluster.lock` rejects concurrent runs.

Results go to the sibling directory
`SwiftBaton-AE-rsocket-bridge-20260928-benchmark-results/` unless
`SB_BENCH_RESULTS` is set. Each trial has `experiment.json`, `driver.log`,
analysis logs, and a pointer to its raw state/images/logs. `plot_all.py`
writes `plots/summary.json` and PNG/PDF figures for successful, validated
trials only. Failed or incomplete trials are listed in `summary.json` as
skipped; they are never substituted with simulated values. The graph reports
median and observed range, with the sample count above each bar. Smoke and
paper-scale trials are plotted separately so their different working-set sizes
cannot be pooled. A YCSB trial is accepted only when failed operations stay
below 1% overall and 0.1% during the fresh-client poststeady check.

The client downtime bar uses the largest gap between successful operations
around cutover where that log is available, otherwise the 10 ms sampled zero
run. TTR50 and TTR100 use a 0.5-second rolling throughput window, a
pre-migration baseline, and a one-second sustained threshold. The zero-run
start is time zero. These definitions and raw timestamps remain in the
analysis JSON so alternative definitions can be recalculated.
`fault_service.png` plots each path's logged mean fault service time and
`summary.json` retains its event count. U and K log service time at different
layers, so that chart is a path-level diagnostic, not an identical kernel
instrumentation point.

## Experiment map

| Directory | Workload and comparison |
| --- | --- |
| `real_world/redis` | 1M × 1 KiB, YCSB-A, 32 clients; U/K |
| `real_world/memcached` | 5M × 1 KiB, YCSB-A, one client; U/K |
| `real_world/voltdb` | 400K records, YCSB-A; U/K |
| `real_world/mysql` | 1M × 1 KiB, YCSB-A; user-requested addition |
| `real_world/largecontainer` | One 4 GiB process, 32 Zipfian range-access threads with reads, writes, short sleep, and per-thread IOPS cap |
| `robustness/zipf` | Redis Zipf skew sweep |
| `robustness/value_size` | Redis 64 B–16 KiB values, approximately fixed logical value bytes |
| `robustness/write_ratio` | Redis read/write proportion sweep |
| `breakdown/downtime_components` | Client gap and CRIU phase timestamps |
| `breakdown/substate_scaling` | Existing extra-memory and FD stress fixtures |
| `breakdown/pretransfer` | Pre-transfer enabled/disabled |
| `breakdown/hot_first` | Hot-first enabled/disabled under 5/25/100 Gbps hardware QoS |
| `breakdown/prefetch` | Prefetch enabled/disabled at two value sizes |
| `breakdown/page_completion` | U/K demand-emphasis comparison with pre-transfer, prefetch, and hot-first off |
| `breakdown/monitor_overhead` | Hot-sampling-path approximation, measured from source throughput |

All cases share the same container setup, client placement, cutover code,
analysis, and rollback path. The runner checks source/target image IDs,
selects the matching U/K CRIU binaries, stages client bindings/helpers, and
restores `/usr/bin/criu` and prior QoS after each trial. For a missing image,
it builds the case Dockerfile on knode2, then streams the image directly to
knode3. LargeContainer is compiled statically at build time; its complete
source and Dockerfile are in `real_world/largecontainer/`.

Every trial programs `ens4f1` on knode2 and knode3 through `mlnx_qos` at the
paper's default 25 Gbps; QoS variants override that rate. The runner records
prior TC1 limits and restores them after each trial. Do not run this suite at
the same time as another network experiment.

## Coverage and interpretation

The MySQL case uses a `MEMORY` table. It measures migration of a live,
RAM-resident SQL workload; it does not validate durable InnoDB data or
filesystem replication. VoltDB stages runtime libraries and persistent path
metadata before the timed workload. Its migration result must be interpreted
as an in-memory workload rather than a general persistent-storage migration.

The 4 GiB and FD substate fixtures currently approximate the corresponding
Fig. 11(b) stresses. The exact 20K-VMA, 4096-UNIX-socket, 512-TCP-socket,
and 300-bind-mount fixtures are not yet implemented in this suite. The
`monitor_overhead` on/off pair changes more than pure A/D-bit collection;
it should not be labeled a pure monitoring-cost measurement. Fig. 15's native
CRIU baseline is also not part of the U/K `page_completion` case. This case
still permits address-order background copy; it is not a strict fault-only
comparison.

Redis, Memcached, LargeContainer, VoltDB, and MySQL have passed U and K smoke
migration. These are functionality checks only. Failed attempts remain
recorded and excluded from plots. A Zipf 1.22 U smoke trial and a 25 Gbps
hot-first U smoke trial have also passed, including QoS restoration. The
demand-emphasis breakdown passed U/K smoke trials, and the 16 KiB prefetch
variant passed a U smoke trial. Run the
paper-scale matrices and replicate each case on both paths before using any
numbers in the paper.
