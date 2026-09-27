# SwiftBaton-K: optional all-range source MR prearm

The sealed default K mode remains at commit `5b41e9e8` and tag
`swiftbaton-k-ae-under50-20260928`. This follow-up removes verbose logging
from frozen source PS validation and legacy dump/restore probes. In the
default mode, the 500,000-record full run `sb_ae_20260928_042848` passed
all AE checks with a **51.43 ms** client success gap. Source PS validation
fell to 0.22 ms, but frozen final MR export varied to 8.25 ms.

The opt-in `--kernel-ps-mr-all` mode preregisters source ranges containing
copied PS pages as well as the other PS layout ranges. Final sparse planning
still covers newly resident gaps. For every final range, the source checks
the pinned MR's notifier and exact PFN snapshot; it validates copied PS
pages against frozen pagemap soft-dirty and PFNs before publishing the final
catalog. A failed check still falls back to frozen registration or aborts.
The normal mode continues to skip PS candidate ranges and preserve useful
PS copies.

The consequence is important: the OFED writable GUP used by preregistration
marks copied PS pages soft-dirty. In all three full trials below, **every
copied PS page was conservatively invalidated**. The PS path transmitted
pages, but contributed zero valid prefetched pages to restoration. These
trials therefore show a downtime/TTR tradeoff for this workload, not a
four-path performance win. The PF, FT, and BG paths remained active.

All trials used 500,000 Redis keys × 10 KiB, 32 YCSB clients, a 64 MiB PS
budget, 25 Gbps hardware QoS, NUMA node 0, and 16 export/catalog/validation
workers. The gap is the intersection of client successful-operation
intervals; TTR90 is the first sustained 1 s at 90% of the target's final
10 s throughput using a 0.5 s smoothing window. Both clocks and the
recovery metric were validated by the AE driver.

| Run | Binary mode | Client gap | Frozen source final prepare | Frozen MR export | TTR90 | Valid PS / copied PS |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| `sb_ae_20260928_043757` | all-range prototype | 49.06 ms | 7.49 ms | 1.41 ms | 2.21 s | 0 / 16,099 |
| `sb_ae_20260928_044103` | all-range prototype | **45.11 ms** | 6.93 ms | 1.43 ms | 2.29 s | 0 / 16,084 |
| `sb_ae_20260928_044944` | explicit opt-in | 47.83 ms | 6.72 ms | 1.36 ms | 2.52 s | 0 / 16,092 |

All three full runs passed target endpoint, 500,000-record presence and
length, 8 MiB bytewise canary, source retirement, K transfer error count
(zero), and post-trial runtime rollback/host health. The record check is not
a bytewise checksum of all values. A smoke run in default mode
(`sb_ae_20260928_044617`) retained 8,457 valid PS pages; opt-in smoke
(`sb_ae_20260928_044806`) passed the same integrity checks. The normal
and catalog fixtures passed. The all-range sample is small and does not
establish a reliable sub-50 ms guarantee.

Run the opt-in mode with:

```sh
python3 scripts/ae/k/prearm_host_trial.py redis --precopy-limit-mb 64 --all-ps-ranges
```

The host trial restores the original CRIU symlinks and source/target kernel
modules. The kernel module itself was not changed by this optimization.
