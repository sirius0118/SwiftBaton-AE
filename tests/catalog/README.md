# Catalog ownership tests

Run `bash tests/catalog/run.sh normal` (also `asan` and `tsan`) on Linux.
The fixtures compile the actual catalog implementation with mocked device ioctls.
They exercise concurrent PS stage/replay, final CONFIG concurrency, allocation and
WATCH_DRAIN errors, zero/partial thread creation fallback, PS slice import,
joining every admitted worker and descriptor cleanup. These are ownership and
concurrency tests; real RDMA and kernel semantics require migration tests.
