# Reproducing the SwiftBaton AE experiments

Run the entry points on knode2 after completing the build, staging, and
preflight steps in the [main README](../README.md). knode1 runs the client;
knode2 and knode3 run the source and destination containers. Each case has
a `case.json` and an executable `run.sh`. A dry run shows the exact driver
commands without changing either host.

```bash
cd /home/k8s/SwiftBaton-AE
experiments/real_world/redis/run.sh --dry-run
experiments/real_world/redis/run.sh --mode u --smoke --trials 1
experiments/real_world/redis/run.sh --mode k --smoke --trials 1
experiments/real_world/redis/run.sh --mode both --trials 5
experiments/robustness/zipf/run.sh --mode both --variant z0p99 --trials 5
python3 experiments/plot_all.py
```

`--smoke` reduces the dataset, thread count and runtime for a functionality
check. Omit it for the case parameters. Run one case at a time; the runner
takes a cluster lock. The benchmark path selects the CRIU binary built from
this checkout. VoltDB and MySQL build an isolated file-lock-capable U/K CRIU
variant from the same checkout; that build never replaces the normal U/K
source or installed binary. The runner checks matching source/target binary
hashes and image IDs. If an image is missing, it builds the case Dockerfile
on knode2 and transfers the image to knode3. LargeContainer is compiled
from `largecontainer.c` during this image build. No image or executable is
stored in Git.

| Directory | Workload or comparison |
| --- | --- |
| `real_world/redis` | Redis, YCSB-A |
| `real_world/memcached` | Memcached, YCSB-A |
| `real_world/voltdb` | VoltDB, YCSB-A |
| `real_world/mysql` | MySQL MEMORY-table workload, YCSB-A |
| `real_world/largecontainer` | Zipfian multithreaded read/write range workload |
| `robustness/zipf` | Redis access skew |
| `robustness/value_size` | Redis value length |
| `robustness/write_ratio` | Redis read/write ratio |
| `breakdown/downtime_components` | Client interruption and migration phases |
| `breakdown/substate_scaling` | Extra memory and file-descriptor stress |
| `breakdown/pretransfer` | Pre-transfer toggle |
| `breakdown/hot_first` | Hot-first toggle and RDMA bandwidth |
| `breakdown/prefetch` | Prefetch toggle |
| `breakdown/page_completion` | Demand-heavy U/K profile |
| `breakdown/monitor_overhead` | Source monitoring-path comparison |

The default is five U and five K trials per variant. Cases may be restricted
with `--mode`, `--variant`, or `--trials`. The runner temporarily configures
ConnectX-6 QoS on both migration hosts, selects CRIU, and restores previous
QoS and CRIU symlinks after each trial. Do not overlap it with another cluster
experiment. Results are written outside Git to
`/home/k8s/SwiftBaton-AE-benchmark-results/` by default, or to
`SB_BENCH_RESULTS` if set. Each trial records its source revision, image and
binary hashes, command, state path, validation, and cleanup outcome.
`plot_all.py` accepts only validated successful trials and writes figures
beside those results. Never commit the generated data or plots.

The MySQL case uses a RAM-resident MEMORY table, so it does not test durable
InnoDB files. The current substate-scaling and monitoring cases approximate
their paper stressors; inspect `case.json` and the generated trial metadata
before mapping a figure to a claim. The page-completion case still permits
address-order background copy. Use `baseline/` for the separate native CRIU,
PCLive, post-copy, hybrid, and remote-fork algorithm profiles.
