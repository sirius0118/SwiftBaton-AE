# Hybrid copy fragmentation check

The hybrid profile uses the U page scheduler's page-indexed validity bitmap
and bounded priority queues. It does not create one pipe for each run of pages
missing after pre-copy. Run `make && ./fragmentation_stress` on Node2 to
check the worst simple fragmentation pattern: alternate pre-copied and missing
pages across a 4 GiB logical address space. The test seeds 524,288 isolated
pre-copy pages, transfers the 524,288 holes through the background lane,
and verifies every page reaches committed state. The file-descriptor count
must remain constant. This tests the scheduler's fragmentation behavior; a
full Redis/YCSB run is still required to measure end-to-end hybrid recovery.

On Node2 the check passed with 1,048,576 pages, 8,462,912 bytes of scheduler
memory, and four file descriptors before and after. The synthetic queue phase
took 0.047 s. These figures are a structural check, not migration performance.

The 500,000 × 10 KiB Redis/YCSB run `sb_ae_20260928_100509` used rsocket
for images, the 6.355 GB PS snapshot and separate AS demand/background lanes.
The PS snapshot arrived in 2.226 s under the 25 Gbps NIC limit. The run
passed all 500,000 indexed key-length checks and the bytewise 8 MiB canary.
Its client-wide success gap was 495.207 ms; the stable target reference was
44,678 operations per second, and TTR90 began 5.840 seconds after service
resumed. Source and target rsocket image phases, snapshot bytes, AS writes,
receives and acknowledgements matched with no transport errors. This is a
single end-to-end trial, not an isolated transport or algorithm speedup.
