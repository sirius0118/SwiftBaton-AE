# Downtime regression fixtures

Run on Linux x86-64 with GCC, Python 3, and passwordless sudo for reading PFNs and the test children's pagemaps:

```sh
bash tests/downtime/run.sh normal
bash tests/downtime/run.sh asan
bash tests/downtime/run.sh tsan
```

These fixtures exercise production C implementations: scheduler bitmap ownership,
parallel frozen pagemap snapshots, sparse range planning, PS dirty/PFN validation,
and inherited stage pages across fork, discard, adoption and COW. The migration
window test excludes terminal workload idle intervals without fabricating samples.

The snapshot fixture uses real child processes and `/proc`, including a forced
unmap race. Only the RDMA export ioctl is stubbed; it validates copied bytes.
Fixtures do not load a kernel module, change networking, or operate on a container.
ASan includes UBSan. The fork/mremap stage fixture is not run under TSan.
Physical U/K migration validation remains a separate requirement; these tests
cannot establish kernel-PTE correctness or application downtime.
