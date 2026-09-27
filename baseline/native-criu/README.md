# Native CRIU over RDMA

`build.sh` pins the unmodified upstream CRIU v3.18 commit
`4c1a2ac41bb80843c927d2fde8f2ff4186f8d278`. It builds in ignored
`.build/upstream-criu` and prints the resulting binary digest. Do not use
`/usr/bin/criu` as evidence of stock CRIU on this cluster: that symlink points
to a modified SwiftBaton build when experiments are idle.

`smoke.sh` dumps a 64 MiB process on Node2, sends every image through the
rsocket relay, restores it on Node3 and checks the original PID, all 16,384
pages and continued counter progress. `container_smoke.sh` switches the CRIU
symlink atomically on both hosts for the owned trial, checkpoints a Redis
container, transfers its entire Docker checkpoint directory over RDMA and
restores the Redis key/value on Node3. Both scripts restore the original CRIU
selection and clean up owned processes/containers on exit. The Docker trial
also takes the same migration lock used by `scripts/run.py`.

Validated 2026-09-28: stock CRIU GitID `v3.18`, binary SHA-256
`cdd7744ee2adbd37292f0778d7d13016e55c17caf430a9d66c3e5aa14a8fbaa2`.
The Redis container smoke transferred 4,144,545 image payload bytes, verified
SHA-256 `8313222ce420f77154d54207a723a431f4340001d04ca5b55d1ff91811696b9e`,
and restored both sentinel and 1,024-byte value. No YCSB performance claim is
made from this smoke.
