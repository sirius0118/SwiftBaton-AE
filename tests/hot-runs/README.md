# PS heat-run preparation

The K source records monotone runs of the immutable sampled address sequence during PS. Finalization merges each run with the final sorted region boundaries, retaining exactly the old per-region heat order. This changes neither page coverage nor dirty validation; hints in unmapped holes are ignored, and new mappings can be served without an old heat hint. Random/short-run input and more than 4096 runs use the original binary-search path. The bounded plan uses at most 96 KiB per sampled process and is freed with its snapshot.

`bash tests/hot-runs/run.sh normal` and `bash tests/hot-runs/run.sh asan` test actual production header code against an independent linear oracle. Cases include both directions, heat bands, arbitrary hint permutations, final holes/changed region boundaries, descriptor order, empty inputs, bounded-capacity fallback, allocation failure, duplicate hints and malformed plan bounds. A large-input diagnostic compares output byte for byte and prints both algorithms' times. These CPU-only timings do not establish migration downtime or TTR improvements.

No kernel, RDMA, page-fault, wire format or target scheduler changes are involved. Full physical migrations remain required.
