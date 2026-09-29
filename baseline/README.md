# Migration baselines

These are runnable algorithm profiles and helpers for the three-node testbed described in the [main README](../README.md). They reuse the artifact's CRIU orchestration and Redis/YCSB workload;
they are prototypes of the listed algorithms, not independent ports of the
original systems.

| Profile | Mechanism |
| --- | --- |
| Native CRIU | Stock CRIU v3.18 stop-and-copy with rsocket image relay |
| PCLive | PS resident snapshot, changed-page round, then demand/background AS |
| Post-copy | Demand and address-order background transfer without PS pages |
| Hybrid-copy | Resident PS with demand and bounded page-index background queues |
| Remote-fork | Kernel demand-only page installation through an rsocket proxy |

Build U/K first with `scripts/build.sh`, stage with `scripts/deploy.py`,
then build and stage the baseline variant needed for a run. For example:

```bash
bash baseline/as-rsocket/build.sh --stage-target   # postcopy, hybrid
bash baseline/pclive/build.sh --stage-target       # pclive
bash baseline/remote-fork/build.sh --stage-target  # remote-fork
python3 baseline/run.py pclive --profile smoke --check
python3 baseline/run.py pclive --profile smoke --execute

make -C baseline/common                        # rsocket image relay
bash baseline/native-criu/build.sh               # upstream CRIU
python3 baseline/native-criu/run_redis.py --help
```

Remote-fork also requires the matching K module with the rsocket-proxy UAPI
feature on Node 3; `--check` reports if it is absent. No baseline script
loads or replaces a kernel module.

The corresponding source and build scripts are under each profile directory.
`baseline/run.py` previews a command unless `--check` or `--execute` is
specified. Run only one experiment at a time. The runner validates binary
identity and data, cleans its owned containers, and restores CRIU selection.
Its output goes to `SB_AE_WORK_ROOT` outside this checkout. Use
`baseline/report.py` on newly generated successful runs; no historical
measurements are included in this repository.
