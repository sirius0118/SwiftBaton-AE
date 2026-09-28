# Demand-only kernel remote-fork baseline

`baseline/run.py remote-fork` selects the isolated `build/criu-K-rsocket`
binary. The destination runs a kernel page-fault provider with
`SBK_DEMAND_ONLY=1`: no PS payload, pre-transfer, prefetch or background
batch transfer occurs. For each fault the provider queues a page request
through `/dev/swiftbaton_k`. A userspace pageclient reads the frozen source
process with `process_vm_readv`, sends the 4 KiB page through librdmacm
`rsocket`/`riowrite`, and completes the kernel request. CRIU image payloads
also use the mapped rsocket transport. The two demand workers own independent
rsocket connections.

The source stays frozen and available until the workload and full Redis
validation finish. The driver stops the target, retires all untouched remote
markers, checks the kernel page totals, and revokes the source. The source and
target rsocket summaries and CRIU image phases must match; `verify_rsocket.py`
checks these against the kernel PF count and rejects non-demand page traffic.
The prototype still creates kernel RDMA endpoints and source registrations
for its inherited catalog/control plane; they do not carry page or image
payloads in this profile. It uses SwiftBaton-K's control plane and is not an
independent implementation of the remote-fork paper.

Run a matched trial with
`python3 baseline/run.py remote-fork --profile redis --threads 16 --duration 300 --warmup 20 --execute`.
Node3 must first load the candidate
`build/module-host-proxy/swiftbaton_k.ko` from this isolated checkout against
its `5.15.167-swiftbaton-k1` kernel. Unload the idle previous module only
after confirming `/sys/module/swiftbaton_k/refcnt` is `0`, then use
`sudo insmod build/module-host-proxy/swiftbaton_k.ko session_dispatch=1 early_prefetch=1 rdma_ack_timeout=12 rdma_retry_count=3`.
The host trial used this candidate temporarily and restored the exact prior
module afterward; the installed on-disk module was not replaced.
The runner checks the CRIU binary on both hosts, installs it only for the
trial, runs full key and canary validation, restores the original CRIU
selection and removes its owned containers. The 100,000 × 1 KiB regression
`sb_ae_20260928_115603` passed with 42,168 demand pages, 172,720,128
rsocket page bytes, matching two-sided image phases, zero transfer errors,
and 811 untouched markers retired after target exit. The 500,000 × 10 KiB,
32-client stress run `sb_ae_20260928_115107` also passed all 500,000 keys
and transferred 1,233,304 demand pages (5,051,613,184 bytes) over rsocket;
that run preceded the image-transport switch, so its image phases used the
legacy native RDMA path and it is not a transport-normalized baseline result.

The baseline K binary extends the source control timeout to 3,600 seconds
so the source remains available through the service period and validation.
The sealed SwiftBaton-K binary is unchanged. The updated proxy module was
validated under KASAN/LOCKDEP in a VM before the Node3 test; the installed
on-disk module is unchanged, and the host test used an isolated candidate.
