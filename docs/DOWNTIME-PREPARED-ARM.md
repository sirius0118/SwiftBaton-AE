# Detached PS preparation for SwiftBaton-K ARM

Status, 2026-09-27: kernel bridge and opt-in page-module API implemented and
validated in isolated native KASAN/RXE guests. **Not yet connected to the CRIU
PS catalog/VMA journal, deployed on the hosts, or measured in a container
migration.** No new client downtime, TTR, or ConnectX demand-fault result is
claimed. The frozen release and host runtimes remain unchanged.

## Reason for this change

The full ordinary-MR run `sb_ae_20260927_213348` spent 217.516 ms in user-observed
ARM. Its 216.559 ms sum of kernel region timings included 20.770 ms token
binding, 135.407 ms bridge work, and 59.707 ms creator-reference release.
Deferring creator release reduced ARM but increased the measured mean physical
cold-fault time by about 0.3–0.4 us and did not improve TTR in those trials.
See `DOWNTIME-CREATOR-DROP.md` for the complete limitations and measurements.

The target application's mm/VMAs do not exist during current PS. Moving the
old ARM call earlier is therefore invalid. A detached plan prepares tokens,
anchors, PTE pages and marker contents without an mm, and transfers ownership
to the final target mm later. Preparation drops creator refs synchronously in
PS, so final ARM needs neither those per-page puts nor a competing worker.

## Kernel bridge

Apply `kernel/patches/linux-5.15.167-sbk-prepared-arm.patch` after the existing
ARM-batch patch. Its base is the actual ARM-batch host source, whose
`mm/swiftbaton_pte.c` SHA-256 is
`88dc2f35ee9396c02fc7c739d8e57a127bac998fe748fa4fd0e3b1ec160cad60`.
Applying the patch to clean copies reproduces all four compiled kernel files.

`sbk_pte_plan_create(start, pages, ids)` reserves each token once and holds the
reference later transferred to one PTE. Token anchors remain immutable even
after cancellation. Partial reservation failure requires discarding the
caller's original IDs; they must not be reused through another ARM. The
shared anchor is published ready only after successful final attachment.

`sbk_pte_plan_arm(plan, current->mm)` is one-shot. It validates the saved address
range against an eligible anonymous VMA and rejects every existing PTE inside
that range. Empty PMDs adopt the prepared PTE pages. Existing normal PTE tables
are retained, with only empty requested entries filled; outside entries stay
untouched. Huge/special/shared/UFFD mappings are rejected as in normal ARM.
Partial failure zaps exactly the published prefix, and destruction skips its
already-transferred references. No existing PTE-table page is replaced.

An anchor stores a base page offset separately from PS-relative token indices.
This supports VMA `vm_pgoff` inherited through the restorer's earlier mremap,
then later fork/split/mremap and background `populate_all` lookups.

Prepared table pages are initially uncharged. Attachment charges the current
adopter's memcg and transfers `NR_PAGETABLE` accounting at the same time; normal
unmap/destruction then uncharges the same cgroup. This is required even when a
different process performed preparation. Global/node page-table accounting
and the normal page-table constructor/destructor are preserved.

`CONFIG_SWIFTBATON_PTE_PLAN_TEST` defaults off and depends on KASAN. Only isolated
tests enable its per-plan partial-publication failure hook. Keep it disabled
in performance and host kernels.

## Page-module API

`SBK_FEATURE_PREPARED_ARM` advertises `SBK_IOC_PREPARE_ANON` on a configured region
fd. The argument is the final target address (`struct sbk_anon_arm`); the region's
page count is already fixed. The session token pool must be reserved before
the first prepare, because binding consumes/seals it as existing ARM does.

Preparation binds provider entries, creates the detached plan, then releases
creator references immediately. It does not publish any PTE or trust any PS
bytes. Existing pretransfer/import and SEAL rules still apply. Final
`SBK_IOC_ARM_ANON` rejects an address mismatch and otherwise attaches the plan.
A prepare/attachment failure after token ownership changes requires a fresh
region fd. File close frees an unused plan before dropping the context's file
reference, breaking the plan-token-context cycle. Active PTEs retain the context
and RDMA transport after close. File-backed mmap cannot bypass a pending plan.

The original ARM path remains the default. On an older kernel without the plan
API, the module advertises no feature and the new ioctl returns EOPNOTSUPP.
No demand-read/provider algorithm, priority, NIC QoS, or speculative-byte
validation is relaxed by this change.

## Validation and measurement scope

Kernel: isolated `linux-5.15.167-arm-plan-kasan`, native RDMA/RXE modules,
KASAN and DEBUG_VM enabled. Image SHA-256:
`95b6ad1fcddc9db449b77fb16be359c8da38968a051dbc702a7125669b5b5ff0`.

The standalone fixture checks cancellation before an mm exists, duplicate and
missing IDs, invalid addresses, occupied/split/shared destinations, first/last
PMD fragments and outside sentinels, partial adopt/copy rollback, preparation
in a parent and attachment in a child, close before first fault, unresolved
fork/COW, mremap/split/background population, discard, and cgroup charge/uncharge.
The successful development run retired all 565,722 created tokens and unloaded
the fixture with no KASAN/BUG/WARNING/Oops. cgroup page-table bytes increased
from 0 to 135,168 on 64 MiB attachment and returned to 0 after unmap.

Two cgroup fixture assertions failed because they read stale batched 5.15
memory statistics. A 2.5-second wait was insufficient: the periodic worker is
deferrable while guest CPUs idle. Both raw failures are retained. The final
isolated-only test hook explicitly flushes normal cgroup rstat before reading
the counters. Two consecutive final runs passed with 135,168 bytes charged and
then zero after unmap. Production accounting and the assertions were unchanged.

Illustrative isolated KASAN timings from the two final standalone runs:

| Range | Detached preparation | Final attachment | Tables adopted |
|---|---:|---:|---:|
| 64 MiB, first | 162.369 ms | 0.190 ms | 32 |
| 64 MiB, repeat | 163.890 ms | 0.186 ms | 32 |
| 1 GiB, first | 2603.090 ms | 1.995 ms | 512 |
| 1 GiB, repeat | 2587.847 ms | 1.211 ms | 512 |

The same 1 GiB fixture's legacy token creation took 2059.139/2038.154 ms and ARM
plus creator puts took 525.660/526.721 ms. These are synthetic, instrumented-kernel
timings, not host speedups, RDMA performance, or end-to-end downtime. Large PS
preparation is still real work and must be scheduled/accounted for in CRIU.

The existing full native RXE regression passed on the changed kernel before
module integration. The new module with prepared ARM enabled also passed its
LOOPBACK/RXE, ordinary/DMA-MR, peer, four-path, fork/COW, source-revoke and drain
tests. A dedicated case prepares before destination mmap, pretransfers pages,
rejects ARM before SEAL, invalidates a modified PS page, closes the fd before
faulting, and checks exact bytes. Unused/canceled prepared contexts and occupied
attachment failure are also checked. All guests shut down normally; their
`12288` taint is the expected external/unsigned test-module taint.

Backward compatibility was also run against the unchanged older native kernel.
Its first full regression failed the existing killed-export-owner fixture: the
parent did not observe a helper before the child exited (the export summary
reported all 32 regions and peak 4). This is retained as a failed attempt, not
a successful cancellation test. An unchanged second run passed the complete
suite including that SIGKILL case. No assertion was weakened or skipped; the
helper-observation fixture still has a scheduling-sensitive coverage window.

Reproduce (against an isolated built kernel with the test option enabled):

```sh
python3 kernel/vm/pte-plan-validation.py --kernel "$KDIR" --output /tmp/sbk-plan-check
python3 kernel/vm/dma-validation.py --kernel "$KDIR" --output /tmp/sbk-plan-rxe --prepared-arm
```

## Required next integration

1. Carry full PS region/layout metadata independently of the current 2 GiB
   precopy data budget; prepare target contexts/plans while the source runs.
2. Integrate the bounded per-range eBPF journal and a stable capture cursor.
   Coalesce affected ranges, validate final maps and page presence, and rebuild
   only eligible changed regions; uncertain/overflow/in-flight observations
   must take the conservative fallback.
3. Do not treat unchanged VMA geometry as proof of unchanged PFNs or data.
   Source-MR PS pinning/reuse needs final PFN/presence checks and existing dirty
   invalidation. Pins/remote-key lifetime cannot be removed merely because no
   mmap syscall occurred.
4. Build and validate the performance/OFED kernel/module combination before
   any host deployment, then measure actual U/K migration downtime, TTR and
   loaded/isolated demand-fault latency. The <50 ms goal remains unmet.
