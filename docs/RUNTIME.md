# Container runtime preparation

On the provided AE testbed, use the existing custom services. The reviewer build/run scripts only switch CRIU for their own experiment and restore its previous selection; they do not rebuild/restart Docker, containerd or runc.

## Docker checkpoint change

The included Docker engine source under `dependencies/docker-ce/components/engine` retains the SwiftBaton checkpoint change in `vendor/github.com/containerd/containerd/task.go`: the caller does not pause/resume the entire container around the checkpoint operation. SwiftBaton's CRIU protocol controls the preparation and final stop. This is required for PS to execute while the application is running. It is a live-migration-specific change, not a general filesystem-consistent checkpoint implementation for arbitrary applications.

The source comes from the previously published artifact's recovered engine tree; its provenance is in `THIRD_PARTY.md`. The executables installed on the provided hosts are not bundled. The containerd and runc development snapshots retained in `dependencies/` differ from the installed containerd 1.5.8 and runc 1.0.3 baselines. Do not describe them as bit-for-bit sources for all running services.

## Independent host provisioning

The engine Dockerfile provides an `ae-runtime` BuildKit target. To
export a candidate bundle without installing programs on the host:

```bash
bash scripts/build-runtime.sh
# Outputs: build/runtime/binary-daemon/
```

This target builds dockerd/docker-proxy and includes the engine builder's
pinned upstream runc (`v1.0.3`), containerd (`v1.5.8`), shims and helpers.
It does not build `dependencies/runc` or `dependencies/containerd` from their
separate development snapshots. Compilation/export of this bundle still needs
validation on a clean build host, followed by checkpoint/restore validation
before treating it as a replacement for the provisioned runtime.

This native rootful target exports the required programs explicitly. It skips
`frozen-images` and the upstream builder's rootless desktop/vpnkit dependencies;
these are not used by the AE migration workflow. APT operations clear cached package indexes,
retry downloads and fail if an index update fails, instead of proceeding with
old indexes. If a package still returns HTTP 404, retain the complete update
log and inspect the configured Debian mirror/proxy. That error alone does not
identify an Ubuntu host or GCC incompatibility. Do not disable signature
verification or silently change the distribution to bypass it.

Build entry points for the included projects are:

```bash
make -C dependencies/runc
make -C dependencies/containerd
make -C dependencies/docker-ce/components/engine binary
(cd dependencies/docker-ce/components/cli && make binary)
```

Use each project's build instructions and matching Go version/development libraries. Docker's engine build is containerized and needs Docker/BuildKit resources; Go vendor layouts and build targets are version-specific. These commands are provided as project entry points, not as a validated replacement for the current testbed runtime stack.

An administrator provisioning new hosts must select a coherent engine/containerd/runc combination, install it into the service's actual executable paths, enable Docker experimental checkpoint support, configure memlock/RDMA access, and verify a basic checkpoint/restore before the SwiftBaton smoke run. Preserve rollback copies of the previous runtime.

SwiftBaton's CRIU reads the per-migration configuration under `/var/lib/criu/migrate_<PID>/`, including `config_ck.cfg` and `config_res.cfg`; the supplied driver creates these. The runtime must support the checkpoint bootstrap and the destination work directory used by this driver. Successful compilation of unrelated runtime versions does not verify that integration.

## Redis image

The reference image's local ID is recorded in `configs/lab.json`; Nodes 2 and 3 of the provided testbed already have it. A local image ID is not a downloadable registry reference. For a new environment, choose a compatible Redis image, pull it on the source and transfer that exact image with `docker save`/`docker load`, or pull an immutable registry digest on both hosts. Export `SB_REDIS_IMAGE=<that ID or digest>` when invoking `scripts/run.py`. Its preflight checks image equality before any workload starts.

The previously used public Redis 7.4.0 reference in `configs/lab.json` is an alternative download reference, not a claim that it is identical to the reference image. Record the image actually selected with any independent reproduction.
