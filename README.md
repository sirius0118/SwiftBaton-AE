# SwiftBaton Artifact Evaluation

Source code for **SwiftBaton: Dependency-Aware Staged Reconstruction for Live Migration of Stateful Containers**, ACM ATC 2026.

The experiment uses three machines: **Node 1** runs the workload client, **Node 2** hosts the source container and coordinates the run, and **Node 3** receives the container. This is a role-based naming scheme; the machines can have any DNS names. The artifact includes **SwiftBaton-U** (userspace page installation) and **SwiftBaton-K** (kernel-assisted page installation). Both use pre-transfer, demand fetches, adjacent prefetch, and hot-first background batches. K also needs a patched kernel on Node 3 and a matching module on Nodes 2 and 3.

Only source and reproduction instructions belong in Git. Compiled programs, checkpoint images, logs, figures, and measurements are generated outside the tracked source. By default, a checkout named `SwiftBaton-AE` writes run data to its sibling directory `SwiftBaton-AE-work`.

**Terms used below:** PS is the preparation stage while the source still runs; CT is the final checkpoint/cutover interval; AS is the stage after the destination resumes service. RDMA MR means a registered memory region, and TTR means time to recover application throughput.

## 1. Contents

| Path | Purpose |
| --- | --- |
| `criu/` | Current SwiftBaton-U CRIU implementation |
| `criu-k/` | Current SwiftBaton-K CRIU integration, including the userspace coordinator and restore plan |
| `kernel/` | Linux 5.15.167 MM/PTE patch, matching configuration, module, UAPI, and isolated kernel build helper |
| `YCSB/` | Modified Java client; millisecond throughput sampling and Redis reconnect/retry behavior |
| `scripts/ae/u/`, `scripts/ae/k/` | Mode-specific migration drivers, cutover, data checks, and scoped cleanup |
| `scripts/` | Build, deployment, execution, and analysis entry points |
| `experiments/` | One-command paper workloads, robustness/breakdown cases, and plotter |
| `baseline/` | Native CRIU and four algorithm-profile baselines |
| `tests/`, `kernel/tests/` | Source-level correctness and safety checks |
| `configs/` | Reference experiment profiles and the provided testbed configuration |
| `dependencies/` | Custom Docker and runtime source trees required for new-host provisioning |
| `Fluid/` | Original management-plane source; the AE runner uses SSH instead |

The U and K CRIU trees are built separately because K adds its own coordinator and memory-management integration. Use the same binary for a mode on Nodes 2 and 3. See [implementation details](docs/IMPLEMENTATION.md), [kernel setup](docs/KERNEL.md), and [component licenses](THIRD_PARTY.md).

## 2. Three-node setup

The following names identify **roles**, not a particular cluster's hostnames.
All commands in this guide run on **Node 2**, unless stated otherwise.

| Node | Role | Address in the provided AE testbed | Kernel requirement |
| --- | --- | --- | --- |
| Node 1 | YCSB or other workload client | `10.0.0.61` | Java and client tools |
| Node 2 | Source container and experiment coordinator | `10.0.0.62` | Linux 5.15.167 in the provided testbed |
| Node 3 | Destination container | `10.0.0.63` | Patched `5.15.167-swiftbaton-k1` for K |

On Node 2, configure SSH aliases for the *roles* using your own hostnames or
addresses. The scripts treat Node 2 as the local machine; they SSH to Node 1
and Node 3. For example, add this to `~/.ssh/config` and replace the bracketed
values:

```sshconfig
Host node1
    HostName <NODE_1_ADDRESS>
    User <SSH_USER>

Host node3
    HostName <NODE_3_ADDRESS>
    User <SSH_USER>
```

Set up key-based authentication and known host keys, then confirm that both
`ssh -o BatchMode=yes node1 true` and `ssh -o BatchMode=yes node3 true`
succeed from Node 2. The account on each node must be able to use Docker and
`sudo -n` for the commands in the runner. Choose an account and checkout
location that resolves to the **same absolute path on all three nodes**; the deployment script stages Nodes 1 and 3.

Nodes 2 and 3 in the provided AE testbed have two NUMA nodes and ConnectX-6
RDMA (device `mlx5_1`, port 1, GID index 3, interface `ens4f1`). The IPs and
device selection are reference settings, not meanings of the Node numbers. The
current migration driver and parts of CRIU use these addresses and RDMA
settings directly. A different cluster needs corresponding code/configuration
changes as well as matching hardware; editing `configs/lab.json` alone is
insufficient. [Runtime provisioning](docs/RUNTIME.md) explains the other
host-level requirements.

Before running, verify:

- Nodes 2 and 3 have the same container image and the same CRIU binary for
  the selected U or K mode.
- Docker experimental checkpoint support and the matching custom
  Docker/containerd/runc stack are available. On the provided testbed, keep
  the existing services; the reviewer scripts do not replace them.
- U has userfaultfd, pagemap/soft-dirty access, page-idle tracking, and BPF
  syscall tracepoints for VMA monitoring.
- K has the patched destination kernel and matching loaded `swiftbaton_k`
  modules on Nodes 2 and 3, with `session_dispatch=1`. Node 2 can keep its
  stock kernel. See [kernel setup](docs/KERNEL.md).
- Python 3.8+, NumPy/Matplotlib, GCC, make, Java 8, Maven, rsync, numactl,
  redis-cli, iptables, conntrack, RDMA tools, and `mlnx_qos` are available.
- Ports 6390, 12346, and 4568 are free. The reference profile uses Docker
  subnet `172.30.52.0/24` and container IP `172.30.52.3`; it also allocates
  an ephemeral client-cutover port. Run only one experiment at a time.
- For the larger Redis profile, reserve at least 16 GiB available RAM per
  migration host, adequate tmpfs, and 10 GiB free for builds/results.
  Building a kernel and OFED needs substantially more disk space.

The supported quickstart uses the **provided, provisioned AE testbed**.
Reviewers can request access through the artifact submission discussion.
The included runtime source snapshots are not bit-for-bit reconstructions of
every installed service (the provided hosts use containerd 1.5.8 and runc
1.0.3). Provisioning a different cluster is a separate administrator task
and has not been validated as a one-command installation path.

## 3. Clone and compile

```bash
cd "$HOME"
git clone https://github.com/sirius0118/SwiftBaton-AE.git
cd SwiftBaton-AE
```

If the checkout already exists, use it instead of cloning over it. For a new Ubuntu 20.04 build host, inspect the package installation commands and run them if needed:

```bash
bash scripts/install-build-deps.sh
bash scripts/install-build-deps.sh --execute
```

Build from source on Node 2:

```bash
bash scripts/build.sh U
bash scripts/build.sh K
bash scripts/build.sh ycsb
bash scripts/build.sh fixture
```

These commands compile in separate copies under `build/`. They do not install system programs, load modules, start containers, or reboot. `SB_BUILD_JOBS` controls compiler parallelism (default 12). Maven needs access to its dependencies on the first build. The client classpath includes `core/target/classes`, `redis/target/classes`, and the dependency JAR directories produced by Maven. Do not substitute upstream YCSB.

Kernel/module builds are documented separately in [docs/KERNEL.md](docs/KERNEL.md). The default K profile requires the prepared-ARM and batch-accounting kernel patches, destination token preparation and unbound-region support, and source PS memory pre-registration. The provided hosts already have the required kernel; their loaded modules must also provide these features. A module with only base K support can complete migration but cannot reproduce this profile's short downtime.

## 4. Stage and inspect

Preview deployment, then stage source and locally built programs from Node 2 to Nodes 1 and 3:

```bash
python3 scripts/deploy.py
python3 scripts/deploy.py --execute
```

Deployment checks that the migration hosts are idle. It leaves daemon services and installed CRIU symlinks unchanged. It copies built programs directly between machines; these files are never added to Git.

The reference Redis image is selected by its immutable local image ID in `configs/lab.json`. If using another compatible image, place the same image on both hosts and set `SB_REDIS_IMAGE` to its immutable ID or digest. The runner checks that it resolves to the same image ID on both hosts. It does not silently pull `latest`.

Check both modes without starting a workload or changing the CRIU selection:

```bash
python3 scripts/run.py U --check
python3 scripts/run.py K --check
```

Each command must exit zero and report `"ok": true`. This checks binary equality, basic host readiness, client build products, image identity, and the loaded module's ABI and capabilities required by the selected profile. The K result includes both module build IDs and capability masks. At actual K startup, the driver additionally checks RDMA device/GID, daemon CRIU resolution and loaded mode. Resolve errors before running an experiment. Rebuilding CRIU or seeing the expected `uname -r` alone does not update or verify the loaded module.

## 5. Minimal end-to-end run

A command without `--execute` or `--check` prints a local plan and does not access peers:

```bash
python3 scripts/run.py U --profile smoke
python3 scripts/run.py K --profile smoke
```

Run each mode sequentially:

```bash
python3 scripts/run.py U --profile smoke --execute
python3 scripts/run.py K --profile smoke --execute
```

The smoke profile uses 100,000 records with a 1,024-byte value, 16 clients, a 75-second workload, a 10-second warmup before migration, and an 8 MiB immutable canary. Reads/updates are each 50%, with the configured YCSB request distribution. Allow several minutes for loading, post-migration measurement, and validation.

The runner:

1. Acquires the local experiment lock and confirms both hosts are idle.
2. Temporarily selects the newly built U or K CRIU on both hosts, then verifies root/Docker/containerd resolve that exact binary.
3. Creates labelled Redis containers, loads data, and starts YCSB on Node 1.
4. Prepares PS state, checkpoints on Node 2, transfers CRIU images through RAM/RDMA, restores on Node 3, and redirects client traffic using experiment-specific rules. K buffers client packets during cutover and uses nftables for the CRIU network lock.
5. Completes page transfer, retires the source, verifies records/sentinel/canary, and analyzes throughput/recovery. K requires all remote PTE markers and background work to drain before source retirement.
6. Verifies image checksums while checkpoint images still exist, cleans the experiment, and restores the previous CRIU symlinks.

Success ends with `SWIFTBATON_AE_PASS mode=U` or `mode=K`. The command prints both:

```text
STATE=<checkout-parent>/SwiftBaton-AE-work/sb_ae_YYYYMMDD_HHMMSS/state.json
RESULT=<checkout-parent>/SwiftBaton-AE-work/driver-U-YYYYMMDD_HHMMSS/result.json
```

A printed state path alone does not indicate success. `result.json` must contain `success: true`, zero driver/analysis/cleanup return codes, and successful restoration on both hosts. The corresponding state must report successful migration, source retirement, and data checks. Keep results outside the source repository.

## 6. Larger Redis experiment and parameters

After smoke passes, run one mode at a time:

```bash
python3 scripts/run.py U --profile redis --execute
python3 scripts/run.py K --profile redis --execute
```

This uses **500,000 records × 10 KiB**, 32 clients, a 90-second workload, and an 8 MiB canary. The total Redis RSS is larger than the raw value size. Exact options are in `configs/profiles.json`.

| Setting | U | K |
| --- | --- | --- |
| NUMA node | 1 | 0 |
| Demand | 1 worker | 2 independent QP/CQ slots |
| Prefetch | 1 worker, window 4 | 2 session workers |
| Background | 4 copy / 4 install workers, 16-page batches | 4 session workers, 32-page RDMA batches |
| PS budget | 8 GiB | 64 MiB; 64 MiB PS chunks |
| Source memory registration | U path | PS pre-registration with invalidation checks; 16 workers for final fallback, 64 MiB chunks |
| Destination preparation | U path | PS token/PTE preparation and early region import; 16 catalog workers |
| Final page validation | 8 workers | 16 workers |
| Cutover | Driver defaults | Buffered client packets, nftables network lock, monitored VMA cache |

These are the current implementation defaults and favor lower fault latency. They are **not a controlled U-versus-K comparison**: NUMA placement, PS budget, and timing boundaries differ. For a controlled comparison, equalize those settings and report the actual parameters. Do not claim a speedup from the default profiles alone.

To change a profile, edit `configs/profiles.json`, stage again, and retain that configuration alongside the generated results. Individual driver options are listed by `python3 scripts/ae/u/run_ae.py --help` and the corresponding K command. The main wrapper provides locking, binary selection, validation and cleanup; invoking a driver directly bypasses that wrapper.

The K profile enables `--kernel-ps-arm --kernel-ps-mr` together. Source mappings are reused only while their invalidation/PFN checks remain valid; changed mappings fall back to final registration. Destination preparation moves allocation and PTE planning into PS. Logs must show `ps_arm_mode ... enabled=1`, `final_prearm ... enabled=1`, and successful prepared target setup. Do not silently disable these options to bypass a module capability error. The separate `--kernel-dma-mr` experiment selects the DMA-key path and disables source PS MR, since those two registration modes are mutually exclusive.

The ordinary K final catalog also coalesces TCP writes and narrows hot-page indices to 16 or 32 bits according to each region's size. The destination restores the same 64-bit index sequence before validation and installation; hint order and dirty-page checks remain unchanged. Both hosts must use matching CRIU binaries. `SB_KERNEL final_wire` records the encoding, byte count and send time. DMA and rsocket catalogs retain their existing wire format.

The hot-index codec has a standalone boundary and malformed-input check that needs no module or container:

```bash
mkdir -p build/tests
gcc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -iquote criu-k/criu/include tests/test_k_hot_wire.c -o build/tests/test_k_hot_wire
./build/tests/test_k_hot_wire
```

## 7. Paper workload and baseline entry points

After both U and K smoke runs pass, reproduce the real-world workload,
robustness, and breakdown cases from [experiments/README.md](experiments/README.md).
For example, preview and then run a Redis case:

```bash
experiments/real_world/redis/run.sh --dry-run
experiments/real_world/redis/run.sh --mode u --smoke --trials 1
experiments/real_world/redis/run.sh --mode k --smoke --trials 1
experiments/real_world/redis/run.sh --mode both --trials 5
python3 experiments/plot_all.py
```

Each case's `case.json` sets the paper-scale parameters; `--smoke` is a
smaller functional check. Missing service images are built from the included
Dockerfiles on Node 2 and copied to Node 3. The scripts use CRIU built from **this
checkout**; VoltDB/MySQL build an isolated variant from the same source for
their file-lock and VMA requirements. Results and figures go to
a sibling directory named `SwiftBaton-AE-benchmark-results/` by default, never inside Git.
The case runner records its source revision, options, hashes, and validations.
The [CPU-resource experiment](experiments/robustness/cpu_limit/README.md) compares U/K Redis migration with normal CRIU affinity and two shared CPUs per migration host, using 1M keys x 1 KiB, Zipf 0.99 and YCSB-A. It records actual affinity and has a dedicated throughput plotter.
Run one case at a time. The [baseline guide](baseline/README.md) describes
native CRIU, PCLive, post-copy, hybrid-copy, and remote-fork profiles.

## 8. RDMA bandwidth limit

The provided testbed uses a 25 Gbps hardware transmit cap on Nodes 2 and 3. An administrator can inspect or change it with:

```bash
sudo mlnx_qos -i ens4f1 -a
sudo mlnx_qos -i ens4f1 --prio_tc=1,1,1,1,1,1,1,1 --ratelimit=0,25,0,0,0,0,0,0
```

Apply the configuration on both migration hosts. The physical link remains 100 Gbps. All priorities share TC1, including ordinary Ethernet traffic on this interface. This is **not** hardware prioritization of demand faults; CPU scheduling and independent QPs are separate mechanisms. The commands do not configure persistence across reboot. Coordinate changes with the cluster owner.

## 9. Results and metric definitions

The wrapper runs the analyzers automatically. To reanalyze a saved run:

```bash
RUN="$(dirname "$PWD")/SwiftBaton-AE-work/sb_ae_YYYYMMDD_HHMMSS"
python3 scripts/analyze_run.py "$RUN"
python3 scripts/analyze_recovery.py "$RUN"
# U transport and fault timing:
python3 scripts/analyze_transport.py "$RUN"
python3 scripts/analyze_faults.py "$RUN"
```

Use the exact run directory printed by the runner. Generated files include throughput CSV/plots, `metrics.json`, `recovery-metrics.json`, raw logs, and mode-specific validation. The log parser assumes UTC+8; keep the documented host timezone consistent.

- **Downtime:** the consecutive zero-throughput client samples are an observed interruption at 10 ms resolution, not an exact CRIU freeze interval. Internal cross-host timestamps require clock-offset measurement before comparing them.
- **TTR50/80/90:** recovery to 50/80/90% of this run's **destination throughput after migration has fully finished**, sustained for one second. Report the stable-reference validity checks and the chosen 100 ms or 500 ms window; the analyzer saves both. Do not substitute source throughput when hosts differ.
- **Full migration time:** checkpoint-command start through verified source retirement, including PS and coordination.
- **U fault latency:** demand enqueue to installation, with all-lane UFFD waits reported separately. **K fault latency:** kernel bridge callback duration, including READY hits/waiting/installation; it is not identical to the count or latency of demand RDMA reads. K histogram quantiles are intervals. Neither measurement is the complete application instruction-to-return time.
- **Correctness:** all indexed keys are checked for presence/length, plus a sentinel and bytewise immutable canary. This is not a full checksum of every concurrently updated YCSB value or proof of application linearizability.

No historical measurements are bundled or substituted for new runs.

## 10. Failure recovery

The wrapper attempts scoped cleanup and restoration even when analysis fails. Inspect its `result.json` and `cleanup.log`. If a run is interrupted before that cleanup finishes, use only its exact state path:

```bash
python3 scripts/ae/u/cleanup_ae.py "$(dirname "$PWD")/SwiftBaton-AE-work/sb_ae_YYYYMMDD_HHMMSS/state.json"
# Use scripts/ae/k/cleanup_ae.py for a K run.
```

These commands execute cleanup; they do not merely preview it. They check recorded ownership, remove the run-specific NAT rule and owned containers/images, and retain logs. The K cleanup destroys the owned destination before dropping source controllers/MRs. Do not use global Docker pruning, `iptables -F`, or `killall criu`.

If `restored` reports an error, inspect `/usr/bin/criu` on both hosts and compare it with `previous` in the wrapper's result. Restore the previous link only after the exact experiment has been cleaned and no active CRIU remains. A concurrently changed link is deliberately not overwritten. Do not restart shared daemons or reboot to solve a routine path mismatch.

## 11. Current scope

The implementation includes all four page-transfer paths and host K migration. The source also includes PS source-MR reuse, batched catalog/ARM work, rsocket proxy transport for a remote-fork baseline, and optional prepared-ARM kernel patches. The provided AE testbed uses its installed kernel/module; building and booting the optional kernel series is a separate administrator path. It does not yet provide NIC hardware demand priority, K adjacent prefetch across MR boundaries, arbitrary FD semantics (including the documented EFD_SEMAPHORE/queued UDP/timerfd restrictions), or automatic source recovery after a fatal migration-controller failure. The K source-retirement protections prevent unsafe reuse of exposed source pages; a fatal controller failure may terminate the source process.

This workflow reproduces the implementation and collects the required metrics. It does not promise every paper figure, a fixed downtime threshold, or a theoretical minimum fault latency. Use the private AE discussion for access or support; provide the failing command and the generated state/result files privately rather than committing them to the repository.
