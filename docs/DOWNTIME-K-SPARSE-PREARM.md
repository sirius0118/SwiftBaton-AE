# K downtime: reuse PS prearm coverage in the final sparse plan

The previous final freeze still read all ~1.75 million Redis virtual pages from
`/proc/PID/pagemap` to discover ~1.56 million data pages, even though PS had
already pinned and captured most of those pages in source read-only MRs.
The new planner partitions the frozen VMA ranges around prearmed MRs whose
notifiers remain valid. It scans pagemap for only the residual ranges, merges
their sparse plan with the trusted ranges in address order, and falls back to
the full scan if any capacity or ordering check fails. It checks the trusted
notifiers again before publishing final MRs. PS candidate ranges still use
the ordinary pagemap and dirty-page validation path. An exact frozen MR can
be reused only after notifier validation; otherwise final export is used.

On one diagnostic 500k-record Redis migration, a full pagemap scan was also
run while the application was frozen. The fast and full plans covered the
same pages: `different=0 missing=0 extra=0 bad_pfns=0` for 1,562,780 data
pages. The diagnostic added ~20 ms to the frozen scan and is not a performance
sample. The diagnostic code was removed after this audit.

All runs used 500,000 records × 10 KiB, 32 YCSB clients, 25 Gbps hardware
QoS, NUMA node 0, 16 export/catalog/validation workers, and a 256 MiB PS
budget. They passed source retirement, endpoint, all-record presence/length,
8 MiB bytewise canary, kernel error counter, and post-trial host health checks.
The all-record check is not a bytewise value checksum.

| Run | Client success gap | Source final prepare | Source sparse scan | Target seal | Recovery |
| --- | ---: | ---: | ---: | ---: | --- |
| `sb_ae_20260928_033110` | 64.45 ms | 14.24 ms | 3.17 ms | 3.41 ms | TTR90 2.39 s, valid |
| `sb_ae_20260928_033419` | 62.82 ms | 14.31 ms | 3.32 ms | 3.43 ms | Analyzer rejected a >50 ms missing observation in destination reference |
| Audit `sb_ae_20260928_034103` | 83.21 ms | 33.76 ms | 23.93 ms | 4.40 ms | Valid; deliberate full scan overhead |

The second run completed migration and Redis validation; its recovery metric
must not be used. The best valid client gap remains 64.45 ms, above the 50 ms
goal. The source sparse scan dropped from roughly 14 ms in previous runs to
roughly 3.2 ms in both non-diagnostic runs. Variation in cutover and restore
still dominates the client gap. No host kernel reboot was needed. The trial
restored original CRIU binaries/modules and verified host health after each
run.
