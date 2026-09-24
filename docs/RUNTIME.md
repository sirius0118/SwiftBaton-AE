# Container runtime preparation

On the prepared AE hosts, use the existing custom services. The reviewer build/run scripts only switch CRIU for their own experiment and restore its previous selection; they do not rebuild/restart Docker, containerd or runc.

## Docker checkpoint change

The included Docker engine source under `dependencies/docker-ce/components/engine` retains the SwiftBaton checkpoint change in `vendor/github.com/containerd/containerd/task.go`: the caller does not pause/resume the entire container around the checkpoint operation. SwiftBaton's CRIU protocol controls the preparation and final stop. This is required for PS to execute while the application is running. It is a live-migration-specific change, not a general filesystem-consistent checkpoint implementation for arbitrary applications.

The source comes from the previously published artifact's recovered engine tree; its provenance is in `THIRD_PARTY.md`. The prepared host executables are not bundled. The containerd and runc development snapshots retained in `dependencies/` differ from the installed containerd 1.5.8 and runc 1.0.3 baselines. Do not describe them as bit-for-bit sources for all running services.

## Independent host provisioning

Build entry points for the included projects are:

```bash
make -C dependencies/runc
make -C dependencies/containerd
make -C dependencies/docker-ce/components/engine binary
(cd dependencies/docker-ce/components/cli && make binary)
```

Use each project's build instructions and matching Go version/development libraries. Docker's engine build is containerized and needs Docker/BuildKit resources; Go vendor layouts and build targets are version-specific. These commands are provided as project entry points, not as a validated replacement for the current prepared runtime stack.

An administrator provisioning new hosts must select a coherent engine/containerd/runc combination, install it into the service's actual executable paths, enable Docker experimental checkpoint support, configure memlock/RDMA access, and verify a basic checkpoint/restore before the SwiftBaton smoke run. Preserve rollback copies of the previous runtime.

SwiftBaton's CRIU reads the per-migration configuration under `/var/lib/criu/migrate_<PID>/`, including `config_ck.cfg` and `config_res.cfg`; the supplied driver creates these. The runtime must support the checkpoint bootstrap and the destination work directory used by this driver. Successful compilation of unrelated runtime versions does not verify that integration.

## Redis image

The prepared image's local ID is recorded in `configs/lab.json`; both hosts already have it. A local image ID is not a downloadable registry reference. For a new environment, choose a compatible Redis image, pull it on the source and transfer that exact image with `docker save`/`docker load`, or pull an immutable registry digest on both hosts. Export `SB_REDIS_IMAGE=<that ID or digest>` when invoking `scripts/run.py`. Its preflight checks image equality before any workload starts.

The previously used public Redis 7.4.0 reference in `configs/lab.json` is an alternative download reference, not a claim that it is identical to the prepared image. Record the image actually selected with any independent reproduction.
