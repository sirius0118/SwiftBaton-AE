# DMA address exchange

Run `bash tests/dma-wire/run.sh normal` and `bash tests/dma-wire/run.sh asan` on Linux.

The production helper exchanges fragmented socket streams through 1M-page vectors,
checks mode/version compatibility, rejects unbounded records, propagates export/import
errors, and never imports a truncated vector. The kernel ioctl is mocked; these tests
do not claim physical RDMA correctness. `kernel/vm/dma-validation.py` and the physical
probe evidence cover that boundary separately.

Python checks K-only option forwarding, capability preflight and evidence from the
executing source/destination, including rejection of missing/mismatched DMA counts.

The opt-in `--kernel-dma-mr` mode negotiates the endpoint reserved byte during PS,
uses PS wire version 5 and final version 3, and appends a full per-page DMA vector
after each nonempty record's index lists. Ordinary mode retains PS version 4 and
final version 2. Kernel page pins and final validation are unchanged.
