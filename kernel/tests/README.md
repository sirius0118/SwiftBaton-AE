# Kernel regression tests

These tests run real token/PTE ownership, anonymous faults, pretransfer,
prefetch, background transfer, source retirement and RDMA catalog paths.
The loopback backend is a correctness fixture; its timings are not hardware
performance results. RXE tests intentionally inject transport failures.

Build from `kernel/` with the matching UAPI:

```sh
gcc -static -O2 -Wall -Wextra -pthread -Iinclude tests/sbk_test.c -o tests/sbk_test
KDIR=/path/to/configured-kasan-kernel SBK_MODULE_PATH=/path/to/swiftbaton_k.ko SBK_TEST_ANON=1 SBK_VM_RAM=2048 bash vm/run.sh
```

The VM runner uses a separately built KASAN / lock-debug kernel with the
anonymous PTE bridge and matching module. It loads RXE only inside that VM.
It verifies the full suite, kernel diagnostics, expected out-of-tree module
taint, and successful module unload. It does not install a host kernel.

Token-pool tests cover bounded reservation, partial ENOMEM, concurrent
reservation, fallback, ARM failure/retry, shared pools across region FDs,
parent-FD close, fork/COW, leftover tokens and chunk-boundary destruction.
The failure-injection parameter defaults to `-1`; it must stay disabled in
performance runs. The retirement test removes all aliases during background
work and checks that the drain acknowledgment joins the work without a false
cancellation error or activity after completion.

Before any host module trial, compare the actual loaded module's
`/sys/module/swiftbaton_k/notes/.note.gnu.build-id` with the ELF note of the
rollback file. `modinfo -n` identifies the installed file, which may differ
from a module loaded manually. Keep its parameters, verify zero references
and no owned migration before switching, and verify the build ID again after
rollback. The trial scripts and observed state are retained in the experiment
archive.
