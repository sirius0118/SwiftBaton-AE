# VMA change journal prototype

This prototype is independent of CRIU and has **not** implemented PS preregistration, destination pre-ARM, or source MR reuse. Node2 native Linux 5.15.167 accepted and ran the BPF programs; they watch only the synthetic child TGID, plus conservative global invalidation for process_madvise/external UFFD ioctls. No kernel modules, boot files, CRIU binaries, persistent BPF pins, or migration settings are changed.

`journal.bpf.c` records syscall number, native x86-64 arguments, result, PID/TID and a unique per-CPU sequence key. Enter/exit update active/generation counters. A bounded hash stores completed events. Any read/map error, incomplete call, counter wrap or capacity overflow prohibits trusting the journal. The userspace consumer requires active=0, poison=0 and a stable generation/count around a complete snapshot. Per-CPU sequence allocation is safe for these nonpreemptible syscall tracepoints and avoids fetch-add-return instructions unsupported by the installed Clang 10. It does not define a total order between concurrent calls; the intended consumer unions conservative invalidation ranges, rather than replaying calls as a memory map.

`delta.h` handles returned mmap addresses, old/new mremap extents including fixed destinations, mprotect/munmap/madvise and protection/lock ranges. Failed operations still invalidate their attempted ranges; failed mmap, brk, exec, clone, ioctl and other unclassified operations invalidate the entire process. External UFFD operations and process_madvise invalidate all watched processes. Bounds arithmetic overflow also forces conservative invalidation.

The native test verified exact arguments/returns, MAP_FIXED, failed mprotect, brk scope, four concurrent threads, a synthetic external UFFD attempt, and bounded-map overflow. Observed 74 initial events (52 thread events, 1 global event); then 352 total events and 96 overflow failures. Raw BPF programs detached and map FDs closed at test completion. Decode cases also check mmap hints and overflowing address arithmetic.

Reproduce on Node2:

```sh
clang -target bpf -O2 -g -Wall -Werror -I/usr/include/x86_64-linux-gnu -c journal.bpf.c -o journal.bpf.o
gcc -O2 -g -Wall -Wextra -Werror -pthread test-journal.c -lbpf -o test-journal
sudo timeout 30 ./test-journal ./journal.bpf.o
```

Before integration:

- The production cache must keep its attach/in-flight barrier, PID identity check, closed process-set/shared-mm restrictions, native ABI guard, and final lightweight maps comparison. Stack growth can change VMAs without the watched syscalls. Unsupported state and observation loss must fall back to full validation.
- Use a capture cursor/session boundary so changes before preparation do not unnecessarily invalidate later preparations. Do not clear maps while calls are in flight or silently discard overflow state.
- The current restorer creates application tasks/VMAs only after END_PROCESS_DUMP. Moving ARM to PS requires a prepared target mm/VMA lifecycle or transferable page-table preparation, not simply invoking the current ioctl earlier.
- Source page pinning and final PFN/presence/dirty validation remain necessary for MR reuse. Unchanged VMA geometry does not guarantee unchanged page identity or contents.
- Prototype probes were tested on Node2 only; no migration performance benefit has yet been measured. The 256-event limit is a deliberate overflow-test bound, not a chosen production capacity.
