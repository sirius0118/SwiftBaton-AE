# SwiftBaton Artifact Evaluation

Source code and experiment scripts for **SwiftBaton: Dependency-Aware Staged Reconstruction for Live Migration of Stateful Containers**, ACM ATC 2026.

This artifact runs a Redis container on a source machine, generates continuous YCSB traffic from a client machine, migrates the container to a destination machine using SwiftBaton-U, and validates the restored application. The workflow includes pre-transfer, demand-fault transfer, adjacent prefetch, and heat-ordered background transfer. A separate stress profile adds COW descendants and dynamic memory mappings.

This repository contains source code and instructions. Build outputs and experiment results are generated locally.

## 1. Source layout

| Path | Contents |
| --- | --- |
| `criu/` | Modified CRIU and the AccessCollector kernel-module source |
| `Fluid/` | Fluid management-plane source |
| `YCSB/` | Modified YCSB, including millisecond sampling and Redis retry handling |
| `dependencies/docker-ce/` | Custom Docker engine and CLI source |
| `dependencies/runc/` | Custom low-level container runtime source |
| `dependencies/containerd/` | Containerd source |
| `DualDriver/script/` | Migration driver, cutover helper, validators, cleanup, and memory fixtures |
| `scripts/` | Build, deployment, experiment profiles, preflight, and analysis scripts |
| `configs/lab.json` | Required topology and Redis image references |

The AE entry point is `scripts/run.sh`. It coordinates Docker and CRIU directly over SSH. Fluid is included for source inspection; starting its legacy API server or agents is not part of this workflow. Use the state-specific cleanup below rather than `Fluid/clean.sh`.

## 2. Machines and prerequisites

Use three x86-64 Linux machines with a working RDMA network. During AE, request an account and a reserved experiment window through the submission's private discussion channel. Run the coordinator commands on **knode2**.

| Role | SSH alias | Hostname | Address |
| --- | --- | --- | --- |
| YCSB client | `knode1` | `skv-node1` | `10.0.0.61` |
| Migration source and coordinator | `knode2` | `skv-node2` | `10.0.0.62` |
| Migration destination | `knode3` | `skv-node3` | `10.0.0.63` |

The supplied driver targets this topology, the directory `/home/k8s/SwiftBaton-AE` on every host, RDMA device `mlx5_1` (port 1), and network interface `ens4f1`. Addresses and device selection also occur in the implementation; editing the JSON alone does not port it to another network.

Requirements:

- Ubuntu 20.04 build environment; source and destination use Linux `5.15.167`, with userfaultfd, privileged pagemap/soft-dirty access, page-idle tracking, and usable BPF syscall tracepoints. Reserve compatible hardware and kernels before evaluation.
- The custom Docker/containerd/runc stack installed on both migration hosts, with Docker experimental mode enabled. Stock Docker is insufficient for the checkpoint/restore coordination used here. See Section 4.
- Noninteractive SSH from knode2 to knode1/knode3, noninteractive `sudo -n`, and Docker access on the migration hosts. Configure SSH aliases and known hosts before running the scripts.
- Java 8 on knode1 and on the build host; Maven on the build host; Python 3.8+, SSH, rsync, redis-cli, iptables, conntrack, RDMA tools, and `mlnx_qos` from the NIC software stack.
- Reserve ports `6390`, `12346`, and `4568`, subnet `172.30.52.0/24`, and container address `172.30.52.3`. The cutover helper also opens an ephemeral TCP listener on knode1.
- Allow at least 10 GiB of free experiment storage and 8 GiB available RAM on each migration host as a starting allowance; additional diagnostic/stress runs may need more. These are planning allowances, not measured minimum requirements.

Run one experiment at a time: the runtime uses shared migration coordination paths. The default image-transfer mode uses RDMA; SSHFS is not needed for the documented profiles.

## 3. Download and build

On knode2, clone into the required directory:

```bash
cd /home/k8s
git clone https://github.com/sirius0118/SwiftBaton-AE.git
cd SwiftBaton-AE
python3 scripts/preflight.py --offline
```

For a machine that needs build dependencies, inspect and then install the Ubuntu packages:

```bash
bash scripts/install-build-deps.sh
bash scripts/install-build-deps.sh --execute
```

The package installer does not provision the kernel, RDMA drivers, or custom container runtime. On an author-provided machine, these prerequisites may already be installed.

Build CRIU, the modified YCSB client and the static memory fixture:

```bash
bash scripts/build.sh all
python3 scripts/preflight.py --offline --built
```

The final check must report `"ok": true`. The build uses two CRIU compiler jobs by default; override with `AE_BUILD_JOBS`. Maven downloads its dependencies and needs network access on the first build. No system binary is replaced, kernel module loaded, or workload started by these commands.

You can rebuild a component separately:

```bash
bash scripts/build.sh criu
bash scripts/build.sh ycsb
bash scripts/build.sh fixture
```

Generated files stay in `criu/`, `YCSB/*/target/`, and `DualDriver/script/memory_fixture` and are ignored by Git. The runner uses these newly built files. Do not substitute an upstream YCSB binary: its sampling and retry behavior differ.

The optional legacy AccessCollector module can be built with `bash scripts/build.sh module`, after installing headers for the running kernel. The documented U profile uses page-idle/soft-dirty tracking and does not install or load this module.

## 4. Container runtime setup

**Author-provided cluster:** use the preconfigured custom Docker/containerd/runc services. Do not replace or restart the shared services to evaluate this artifact.

**Independent cluster:** an administrator must build and provision the custom runtime sources before following the remaining sections. Component build entry points are:

```bash
# Compile runc; requires Go and libseccomp development headers.
make -C dependencies/runc

# Compile containerd; see its BUILDING.md for the required Go toolchain.
make -C dependencies/containerd

# Compile the Docker engine using its containerized build environment.
make -C dependencies/docker-ce/components/engine binary

# Compile the Docker CLI using Docker Buildx.
(cd dependencies/docker-ce/components/cli && docker buildx bake)
```

Use the included sources and component build documentation. Install the resulting binaries into the paths used by the host's Docker service, configure Docker experimental mode, and provision RDMA/memlock and kernel support. This is an administrator setup step, separate from running the experiment. The repository does not automatically overwrite Docker services.

The custom runtime must support the coordination files under `/var/lib/criu/migrate_<PID>/`, including `config_ck.cfg` and `config_res.cfg`. The prepared-cluster quickstart assumes this integration is already installed; successful compilation alone does not establish that a fresh cluster is configured correctly.

## 5. Deploy and prepare the workload image

Preview commands omit `--execute`. Run the executing forms only in your reserved machine window.

```bash
# Read-only inspection; does not require the package on peers yet.
python3 scripts/preflight.py --inspect

# Copy sources and locally built CRIU/YCSB/fixture to knode1 and knode3.
python3 scripts/deploy.py stage
python3 scripts/deploy.py stage --execute

# Download the pinned Redis 7.4.0 amd64 image and copy it to knode3.
python3 scripts/deploy.py image
python3 scripts/deploy.py image --execute
```

The image is an immutable public Docker Hub reference in `configs/lab.json`. The script checks its image ID and transfers the same image with `docker save`/`docker load`. No Redis container is started during image preparation.

Select the CRIU binary compiled from this checkout:

```bash
python3 scripts/deploy.py activate
python3 scripts/deploy.py activate --execute
python3 scripts/preflight.py --ready
```

Activation changes the existing `/usr/bin/criu` symlink on knode2/knode3 and records its previous target in `runtime-backup/`. It refuses to replace a regular system file or proceed with active CRIU processes or retained AE containers. On a fresh cluster, the administrator must first arrange this runtime integration. Preflight checks that installed and deployed CRIU binaries match the locally built binary, that the image is present, and that required services, kernel features and ports are available. Resolve every error before proceeding.

## 6. Run the minimal migration

Preview the complete profile:

```bash
bash scripts/run.sh smoke
```

Start the workload and real container migration:

```bash
bash scripts/run.sh smoke --execute
```

This loads 100,000 records with a 1,024-byte field, starts a 45-second workload with 16 client threads, waits 10 seconds before migration, and includes an 8 MiB immutable canary. Reads and updates each account for 50% of operations, using a Zipfian distribution. Allow several minutes for loading and validation in addition to the workload duration.

The driver creates labelled containers, loads Redis, starts YCSB on knode1, checkpoints the source, transfers images over RDMA, restores on knode3, completes page transfer and redirects client traffic. It retires the source and checks the destination data. It does not automatically delete the run's resources, so checkpoint images remain available for verification.

At exit, the driver prints:

```text
STATE=/home/k8s/SwiftBaton-AE/ae-work/sb_ae_YYYYMMDD_HHMMSS/state.json
```

Copy the **exact printed path** for verification and cleanup. A state path is also printed after a failed run and does not by itself indicate success.

## 7. Verify the result

Set `STATE` to the path printed by your run. Verify images **before cleanup**, while source/destination checkpoint files still exist:

```bash
STATE=/home/k8s/SwiftBaton-AE/ae-work/sb_ae_YYYYMMDD_HHMMSS/state.json
python3 DualDriver/script/verify_images.py "$STATE" > "$(dirname "$STATE")/image-verification.json"
python3 scripts/check_result.py "$STATE"
```

Expected results:

- `check_result.py` exits zero and prints `"ok": true`.
- The state contains `success: true` and `source_retired: true`.
- Every requested record is checked, with zero missing or wrong-length values; the sentinel is correct.
- The immutable canary passes bytewise verification.
- Checkpoint image hashes match, with `equal: true` and `files_checked > 0`.
- For the stress profile, memory-child outcomes also pass.

The record scan validates presence and field length; it does not checksum every concurrently updated value or establish full application linearizability. The client includes reconnect/retry handling. This artifact's scope is the U migration workflow and correctness checks; the commands do not reproduce every paper figure or the K path.

## 8. Larger and stress experiments

After verifying and cleaning the previous run, select one profile:

| Profile | Records | Duration | Warmup | Threads | Additional coverage |
| --- | ---: | ---: | ---: | ---: | --- |
| `smoke` | 100,000 | 45 s | 10 s | 16 | 8 MiB canary |
| `redis` | 1,000,000 | 90 s | 15 s | 32 | 8 MiB canary |
| `stress` | 1,000,000 | 90 s | 15 s | 32 | Canary, 3 memory parents, 3 COW descendants, dynamic mappings |

```bash
bash scripts/run.sh redis --execute
# Verify and clean that run before starting the next one.
bash scripts/run.sh stress --execute
```

Omit `--execute` to preview either command. The stress profile uses 64 MiB and four workers per memory parent and exercises changing mappings and fork/COW behavior. See `python3 DualDriver/script/run_ae.py --help` for individual options.

To analyze your newly generated results:

```bash
python3 -m venv .venv
.venv/bin/pip install -r requirements-analysis.txt
.venv/bin/python scripts/analyze_run.py "$(dirname "$STATE")"
python3 scripts/analyze_phases.py "$(dirname "$STATE")"
```

Outputs include throughput CSVs, `metrics.json`, `throughput.png`, and internal phase measurements. The log analysis assumes UTC+8 timestamps; use consistent host timezones. Zero-throughput sampling intervals are not exact CRIU freeze durations. No reference experiment records are bundled.

## 9. Cleanup and restore

Preview, then clean only your run using its exact state file:

```bash
python3 DualDriver/script/cleanup_ae.py "$STATE"
python3 DualDriver/script/cleanup_ae.py "$STATE" --execute
```

Cleanup uses recorded process IDs, labelled containers, owned mounts and the run-specific NAT rule. Results remain in `ae-work/`. The shared Docker network and global CRIU logs may remain for reuse. Use this procedure after a failed run as well, and inspect any reported cleanup errors before another experiment.

After all your runs are cleaned and the nodes are idle, restore the previous CRIU symlinks:

```bash
python3 scripts/deploy.py restore
python3 scripts/deploy.py restore --execute
```

If a symlink has been changed since activation, restoration stops for manual inspection.

## 10. Troubleshooting and support

- **Missing classes/binaries:** run `scripts/build.sh all`, then repeat staging. The public checkout contains no precompiled binaries.
- **Maven download failures:** check network access and Java 8; rerun the YCSB build. Maven offline mode works only after all dependencies have been cached.
- **Readiness failure:** inspect the reported host and prerequisite. A running CRIU process or retained AE container can belong to another experiment; reserve the nodes and clean only your own run.
- **Image missing:** rerun the image preparation step. Source and destination must use the same configured image ID.
- **Dump/restore failure:** retain `state.json`, driver output and the dump/restore/pageclient logs in your run directory. Check custom-runtime installation, kernel features and RDMA connectivity.
- **Image verification failure after cleanup:** image verification must precede cleanup because it reads the live checkpoint files.

Use the AE submission's private discussion channel for machine access and evaluation support; public code issues can be filed in this repository. Preserve upstream licenses and notices; see [THIRD_PARTY.md](THIRD_PARTY.md).
