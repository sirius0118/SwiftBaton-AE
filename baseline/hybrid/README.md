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
