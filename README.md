# SwiftBaton Artifact Evaluation

Source code for **SwiftBaton: Dependency-Aware Staged Reconstruction for Live Migration of Stateful Containers**, ACM ATC 2026.

This artifact migrates a running Redis container from **knode2 to knode3**, while the modified YCSB client on **knode1** continues issuing requests. It includes both **SwiftBaton-U** and **SwiftBaton-K**, with pre-transfer, demand-fault transfer, adjacent prefetch, and hot-first background batch transfer. K uses a patched host kernel and a loadable module for direct RDMA reads and anonymous-page installation.

The repository contains source and instructions. It does not contain precompiled programs, historical measurements, checkpoint images, or development reports. Build products are generated under `build/` and ignored by Git. Run results are written **outside the repository**, by default to `/home/k8s/SwiftBaton-AE-work/`.

## 1. Contents

| Path | Purpose |
| --- | --- |
| `criu/` | Current SwiftBaton-U CRIU implementation |
| `criu-k/` | Current SwiftBaton-K CRIU integration, including the userspace coordinator and restore plan |
| `kernel/` | Linux 5.15.167 MM/PTE patch, matching configuration, module, UAPI, and isolated kernel build helper |
| `YCSB/` | Modified Java client; millisecond throughput sampling and Redis reconnect/retry behavior |
| `scripts/ae/u/`, `scripts/ae/k/` | Mode-specific migration drivers, cutover, data checks, and scoped cleanup |
| `scripts/` | Build, deployment, execution, and analysis entry points |
| `configs/` | Accepted profiles and the prepared-cluster topology |
| `dependencies/` | Existing custom Docker and runtime source trees |
| `Fluid/` | Original management-plane source; not needed for this SSH-based AE entry point |

The two CRIU trees intentionally remain separate: the U implementation stays unchanged while K adds its own coordinator and MM integration. Do not mix binaries from different trees or builds between hosts. See [implementation details](docs/IMPLEMENTATION.md), [kernel setup](docs/KERNEL.md), and [component licenses](THIRD_PARTY.md).

## 2. Prepared cluster

Request access and a reserved three-node time window through the artifact submission's private discussion channel. Run the following coordinator commands on **knode2** as the prepared `k8s` account.

| Role | SSH alias | Address | Current kernel |
| --- | --- | --- | --- |
| Client | `knode1` | `10.0.0.61` | Java workload host |
| Source | `knode2` | `10.0.0.62` | `5.15.167` |
| Destination | `knode3` | `10.0.0.63` | `5.15.167-swiftbaton-k1` |

The current setup has two NUMA nodes and ConnectX-6 RDMA, device `mlx5_1`, port 1, GID index 3, interface `ens4f1`. The driver and parts of U's transport use this fixed lab topology; editing `configs/lab.json` alone does not port the implementation. Both migration hosts need the same container image and the same CRIU binary for the selected mode.

Prerequisites:

- Noninteractive SSH from knode2 to knode1/knode3, known host keys, `sudo -n`, and Docker access.
- The prepared custom Docker/containerd/runc stack and Docker experimental checkpoint support. Do not replace the shared services for a reviewer run.
- For U: userfaultfd, privileged pagemap/soft-dirty access, page-idle tracking, and BPF syscall tracepoints for the selected VMA-cache profile. The older AccessCollector module is not required by this profile.
- For K: the patched destination kernel and matching, already-loaded `swiftbaton_k` module on both migration hosts, with `session_dispatch=1`. The source may retain its stock kernel. [Kernel setup](docs/KERNEL.md) is an administrator provisioning step.
- Python 3.8+, NumPy/Matplotlib, GCC, make, Java 8, Maven, rsync, numactl, redis-cli, iptables, conntrack, RDMA tools and `mlnx_qos`.
- Ports 6390, 12346 and 4568, Docker subnet `172.30.52.0/24`, and container IP `172.30.52.3`; an ephemeral client cutover port is also used. Run one experiment at a time.
- For the large profile, reserve at least 16 GiB available RAM per migration host and adequate tmpfs space. Keep at least 10 GiB free for local build/results; building a complete kernel/OFED tree needs substantially more disk space.

The prepared hosts use containerd 1.5.8 and runc 1.0.3. The development runtime source baselines included by the original artifact are not exact reconstructions of every installed daemon. The **supported reviewer quickstart is the prepared cluster**. For a new cluster, see [runtime provisioning](docs/RUNTIME.md); fresh-cluster installation is not an automatically validated path.

## 3. Clone and compile

```bash
cd /home/k8s
git clone https://github.com/sirius0118/SwiftBaton-AE.git
cd SwiftBaton-AE
```

If the prepared directory already exists, use it instead of cloning over it. For a new Ubuntu 20.04 build host, inspect the package installation commands and run them if needed:

```bash
bash scripts/install-build-deps.sh
bash scripts/install-build-deps.sh --execute
```

Build from source on knode2:

```bash
bash scripts/build.sh U
bash scripts/build.sh K
bash scripts/build.sh ycsb
bash scripts/build.sh fixture
```

These commands compile in separate copies under `build/`. They do not install system programs, load modules, start containers, or reboot. `SB_BUILD_JOBS` controls compiler parallelism (default 12). Maven needs access to its dependencies on the first build. The client classpath includes `core/target/classes`, `redis/target/classes`, and the dependency JAR directories produced by Maven. Do not substitute upstream YCSB.

Kernel/module builds are documented separately in [docs/KERNEL.md](docs/KERNEL.md). They are unnecessary for a reviewer using the already-provisioned K hosts.

## 4. Stage and inspect

Use the same absolute repository path on all three hosts. Preview, then copy source and locally compiled programs to knode1 and knode3:

```bash
python3 scripts/deploy.py
python3 scripts/deploy.py --execute
```

Deployment checks that the migration hosts are idle. It leaves daemon services and installed CRIU symlinks unchanged. It copies built programs directly between machines; these files are never added to Git.

The prepared Redis image is selected by its immutable local image ID in `configs/lab.json`. If using another compatible image, place the same image on both hosts and set `SB_REDIS_IMAGE` to its immutable ID or digest. The runner checks that it resolves to the same image ID on both hosts. It does not silently pull `latest`.

Check both modes without starting a workload or changing the CRIU selection:

```bash
python3 scripts/run.py U --check
python3 scripts/run.py K --check
```

Each command must exit zero and report `"ok": true`. This checks binary equality, basic host readiness, client build products, image identity, and module availability. At actual K startup, the driver additionally checks the kernel UAPI/features, RDMA device/GID and loaded mode. Resolve errors before running an experiment.

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

The smoke profile uses 100,000 records with a 1,024-byte value, 16 clients, a 45-second workload, a 10-second warmup before migration, and an 8 MiB immutable canary. Reads/updates are each 50%, with the configured YCSB request distribution. Allow several minutes for loading, post-migration measurement, and validation.

The runner:

1. Acquires the local experiment lock and confirms both hosts are idle.
2. Temporarily selects the newly built U or K CRIU on both hosts, then verifies root/Docker/containerd resolve that exact binary.
3. Creates labelled Redis containers, loads data, and starts YCSB on knode1.
4. Prepares PS state, checkpoints on knode2, transfers CRIU images through RAM/RDMA, restores on knode3, and redirects client traffic with an experiment-specific iptables rule.
5. Completes page transfer, retires the source, verifies records/sentinel/canary, and analyzes throughput/recovery. K requires all remote PTE markers and background work to drain before source retirement.
6. Verifies image checksums while checkpoint images still exist, cleans the experiment, and restores the previous CRIU symlinks.

Success ends with `SWIFTBATON_AE_PASS mode=U` or `mode=K`. The command prints both:

```text
STATE=/home/k8s/SwiftBaton-AE-work/sb_ae_YYYYMMDD_HHMMSS/state.json
RESULT=/home/k8s/SwiftBaton-AE-work/driver-U-YYYYMMDD_HHMMSS/result.json
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
| PS budget | 8 GiB | 2 GiB; 64 MiB PS chunks |
| Final MR registration | U path | 4 workers |

These retain each implementation's accepted configuration and favor lower fault latency. They are **not a controlled U-versus-K comparison**: NUMA placement, PS budget, and timing boundaries differ. For a controlled comparison, equalize those settings and report the actual parameters. Do not claim a speedup from the default profiles alone.

To change a profile, edit `configs/profiles.json`, stage again, and retain that configuration alongside the generated results. Individual driver options are listed by `python3 scripts/ae/u/run_ae.py --help` and the corresponding K command. The main wrapper provides locking, binary selection, validation and cleanup; invoking a driver directly bypasses that wrapper.

## 7. RDMA bandwidth limit

The prepared cluster uses a 25 Gbps hardware transmit cap on both hosts. An administrator can inspect or change it with:

```bash
sudo mlnx_qos -i ens4f1 -a
sudo mlnx_qos -i ens4f1 --prio_tc=1,1,1,1,1,1,1,1 --ratelimit=0,25,0,0,0,0,0,0
```

Apply the configuration on both migration hosts. The physical link remains 100 Gbps. All priorities share TC1, including ordinary Ethernet traffic on this interface. This is **not** hardware prioritization of demand faults; CPU scheduling and independent QPs are separate mechanisms. The commands do not configure persistence across reboot. Coordinate changes with the cluster owner.

## 8. Results and metric definitions

The wrapper runs the analyzers automatically. To reanalyze a saved run:

```bash
RUN=/home/k8s/SwiftBaton-AE-work/sb_ae_YYYYMMDD_HHMMSS
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

## 9. Failure recovery

The wrapper attempts scoped cleanup and restoration even when analysis fails. Inspect its `result.json` and `cleanup.log`. If a run is interrupted before that cleanup finishes, use only its exact state path:

```bash
python3 scripts/ae/u/cleanup_ae.py /home/k8s/SwiftBaton-AE-work/sb_ae_YYYYMMDD_HHMMSS/state.json
# Use scripts/ae/k/cleanup_ae.py for a K run.
```

These commands execute cleanup; they do not merely preview it. They check recorded ownership, remove the run-specific NAT rule and owned containers/images, and retain logs. The K cleanup destroys the owned destination before dropping source controllers/MRs. Do not use global Docker pruning, `iptables -F`, or `killall criu`.

If `restored` reports an error, inspect `/usr/bin/criu` on both hosts and compare it with `previous` in the wrapper's result. Restore the previous link only after the exact experiment has been cleaned and no active CRIU remains. A concurrently changed link is deliberately not overwritten. Do not restart shared daemons or reboot to solve a routine path mismatch.

## 10. Current scope

The implementation includes all four page-transfer paths and host K migration. It does not yet provide NIC hardware demand priority, K adjacent prefetch across MR boundaries, arbitrary FD semantics (including the documented EFD_SEMAPHORE/queued UDP/timerfd restrictions), or automatic source recovery after a fatal migration-controller failure. The K source-retirement protections prevent unsafe reuse of exposed source pages; a fatal controller failure may terminate the source process.

This workflow reproduces the implementation and collects the required metrics. It does not promise every paper figure, a fixed downtime threshold, or a theoretical minimum fault latency. Use the private AE discussion for access or support; provide the failing command and the generated state/result files privately rather than committing them to the repository.
