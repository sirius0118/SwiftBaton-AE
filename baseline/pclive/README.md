# PCLive resident refresh baseline

Select `pclive` through `baseline/run.py`. It uses the separate
`build/criu-U-pclive/criu/criu` binary; the sealed U/K binaries are unchanged.
Build and stage it from Node2 with `bash baseline/pclive/build.sh --stage-target`.

During PS, the first snapshot is copied into anonymous memory owned by the
destination restore parent. The source then re-arms soft-dirty tracking and
re-copies every candidate into the *same* source MR. The target pageclient
RDMA-reads it into the *same* target memfd, and the restore parent overwrites
the existing resident anonymous pages in place. A marker is published only
after the RDMA completion; the restore parent checks the snapshot nonce,
page count and address/VMA identity before applying it. PS and final IS
validity manifests still decide which pages may be adopted with `mremap`.
Any page that cannot be recopied or fails final validation falls back to the
ordinary pageclient path.

The second round deliberately copies all candidate pages. A dirty-only round
based solely on soft-dirty could lose a write between scanning the old epoch
and clearing it, then adopt stale data. A future dirty-delta implementation
needs an atomic write-protection handoff. This baseline implements two real
resident snapshots, while additional rounds and dirty-only deltas remain
separate work.

The 100k x 1 KiB Redis/YCSB smoke run `sb_ae_20260928_065430` passed full
key-length and bytewise 8 MiB canary validation. Logs show 31,883 pages
recopied at the source, 31,883 pages refreshed in the destination resident
stage, and a second pageclient RDMA read. Its client-wide successful-operation
gap was 108.101 ms. The 500k x 10 KiB run `sb_ae_20260928_065735` also
passed all indexed key-length and canary checks. The second source round
recopied 1,534,213 pages; the destination refreshed the same number of
resident anonymous pages (6.28 GB). Its client-wide successful-operation
gap was 362.427 ms; the stable target reference was 44.3k ops/s and TTR90
began 31.92 seconds after client service resumed. This long recovery reflects
the baseline's full second snapshot and later page completion; it is not a
SwiftBaton-U downtime measurement.
