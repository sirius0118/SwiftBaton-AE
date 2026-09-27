# K anonymous marker activation: batched MM bridge

Status: development. Do not infer an end-to-end downtime result from an ARM
microbenchmark. The last measured physical-container K downtime remains
638–650 ms until this kernel is actually deployed and a complete migration is
measured. The 56 ms U release and previous candidate are preserved.

Apply `kernel/patches/linux-5.15.167-sbk-arm-batch.patch` after the existing K1
PTE patch. It retains the original ABI and fault handler and changes setup only:

1. Acquire token references in bounded batches of 64 under the XArray lock.
   No lock is held over the entire region; rescheduling remains possible.
2. Walk the upper tables once per PMD and inspect a PTE page under one PTE lock.
   Reject existing PTEs and huge/special mappings before inserting markers.
3. Transfer the acquired lookup reference to each installed marker, instead of
   incrementing and then decrementing a second reference for every page.
4. Add the shared anonymous-anchor references in one operation while holding
   `mmap_write_lock`, before any rollback or unlock can permit retirement.

Caller-owned creator references remain unchanged. Missing lookups release every
reference already acquired. Partial insertion rolls back only the installed
prefix. The duplicate-token test explicitly exercises this path. Existing PTEs
are never overwritten. No application page validity check is omitted.

`arm_timing=1` on the development module prints allocation, token binding,
bridge and creator-reference-release durations; it is off by default. Serial
console output adds overhead to the outer ioctl timer. Do not use a timed
logging run to claim the production wall time.

`kernel/tests/arm-bench.c` measures CONFIG, reservation and ARM separately,
checks sampled page contents and exact retirement, and labels its result as a
setup microbenchmark. It does not test RDMA or container/client availability.
`kernel/vm/arm-validation.py` builds the real production and bridge fixture
modules, boots only an isolated kernel, checks the original full anonymous
suite and the new ownership/rollback fixture, and rejects kernel warnings,
unexpected taint or failed module unload. Without `--ofed`, it also runs RXE.
See `kernel/tests/bridge/README.md` for exact coverage and the initial fixture
command-number correction.

Example (all paths supplied explicitly):

```sh
python3 kernel/vm/arm-validation.py --kernel /path/to/patched/kernel \
  --output /path/to/new/validation-directory
```

Use a KASAN/lock-debug kernel for safety tests. For a matching performance kernel
and OFED build, add `--ofed /path/to/ofed --bench`. Do not run heavy builds at the
same time as the before/after timing trials.

## Isolated before/after results

The performance and KASAN/RXE kernels passed the full suite plus the rollback
fixture. Performance trials ran in A-B-B-A kernel order with timing logs off;
each boot measured three trials per size. No heavy build ran concurrently.

| Range | K1 ARM median (6 trials) | Batched ARM median (6 trials) | Reduction |
|---|---:|---:|---:|
| 64 MiB / 16,384 pages | 2.226666 ms | 1.394369 ms | 37.38% |
| 1 GiB / 262,144 pages | 82.145421 ms | 42.494114 ms | 48.27% |

These remain VM setup microbenchmarks, not measured client downtime. The entire
kernel configuration and all 11,981 kernel-export CRCs match K1. Candidate
bzImage SHA-256 is `4268bfc5f5eda3bd13ffbb9cfb7969062c8acfbbfbf3d12c9cd858880c12504e`;
its release string remains `5.15.167-swiftbaton-k1` for the unchanged module ABI,
so physical validation must check the boot image and ELF notes, not just uname.
The candidate uses a separate one-shot boot entry; neither the baseline image
nor the original default entry is overwritten.
