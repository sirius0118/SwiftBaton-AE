# SwiftBaton-K source PS MR preregistration

This opt-in branch moves part of source RDMA memory registration into PS. It
does not change the frozen final catalog's authority over page coverage. The
client-observed gap on two 500,000-record Redis runs was **122.00 and
118.17 ms**, compared with 169.56 and 172.83 ms on the preceding prepared ARM
checkpoint. The requested <50 ms full-workload goal remains open.

The source first collects advisory resident ranges during PS, then registers
only ranges without copied PS pages. This avoids writable GUP from setting
soft-dirty after the PS snapshot. Each preregistered range has an MMU interval
invalidation notifier; the source records exact pagemap PFNs after registration.
After freezing, final sparse planning and PS dirty/PFN validation still run.
A region is reused only when its PID, address, length, every PFN, and notifier
status match. Any mismatch uses the original frozen-source registration path.
The session owns all successful or partially successful MRs until source
revocation; the notifier is removed after MR deregistration. Remote-mm
registration requires CAP_SYS_ADMIN and always runs in a joined kernel helper.

`--kernel-ps-mr` requires K, PS ARM, sparse planning and ordinary MRs. Docker
checkpoint/restore uses CRIU's separate service config parser, so both service
branches now recognize the option. The AE driver checks the executing source
log and final reuse accounting; a silently dropped option fails validation.

| Workload | Client gap | Source PS registration | Reused/final regions | Frozen source final preparation | Frozen source MR export | Target final catalog seal | TTR90 start |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 100k × 1 KiB smoke | 37.02 ms | 3.47 ms | 176/272 | 7.75 ms | 5.14 ms | 2.52 ms | 0.00 s |
| 500k × 10 KiB, run 1 | 122.00 ms | 391.54 ms | 298/419 | 57.05 ms | 26.93 ms | 13.50 ms | 2.09 s |
| 500k × 10 KiB, run 2 | 118.17 ms | 426.78 ms | 242/413 | 55.60 ms | 27.82 ms | 12.25 ms | 1.77 s |

The PS registration time is outside the stop interval. Run 1 registered 319
advisory ranges and retained 298 valid PFN snapshots; run 2 registered 318
and retained 242. Regions that changed, raced, or could not be verified fell
back safely. The source final scan was 13.79/12.03 ms, PS validation
5.79/4.70 ms, and hot ordering 10.19/10.69 ms in the two full runs. The
remaining non-source portion of the client gap is substantial; phase durations
cannot be added as a precise gap decomposition because work overlaps and the
client metric samples successful responses.

Both full runs passed key count, presence and expected length for 500,000
indexed records; the 8 MiB canary passed bytewise, client endpoint checks
passed, transfer errors were zero, and PF/FT/BG/PS paths all moved pages. TTR90
uses the target-final 10 s throughput reference, 0.5 s smoothing and a
sustained 1 s 90% threshold. Each full run logged 32 successful reconnect
calls, so transparent TCP continuity is not established. The isolated cold
page-fault timing remains the earlier 9.1–9.2 µs mean physical probe; these
trials did not remeasure its distribution under concurrent transfer.

The module and source CRIU compile; the KASAN/RXE VM passed remote-mm
prearm, unmap invalidation, revoke and existing K regression paths. The host
smoke and two full runs passed the AE validators. The first host attempt
failed before migration because the new isolated script directory had not
been staged on Node1; its AE resources were cleaned and both original
runtimes recovered before the successful trials. After each successful trial,
both original module build IDs, CRIU links, boot IDs, 25 Gbps QoS and the
absence of owned AE resources passed the health check. The prior release
branch and host default boot entry were not modified.

Raw trial directories and full counters are recorded in
`docs/source-ps-mr-trials.json`. A later experiment may test registering all
source ranges before the PS soft-dirty epoch begins, but must first prove
that clearing soft-dirty does not invalidate the preregistered MRs. It must
retain the frozen final pagemap scan: new resident pages within an existing
VMA are not covered by VMA syscall monitoring alone.
