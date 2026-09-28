# PCLive resident refresh baseline

Select `pclive` through `baseline/run.py`. It uses the separate
`build/criu-U-pclive/criu/criu` binary; the sealed U/K binaries are unchanged.
Build and stage it from Node2 with `bash baseline/pclive/build.sh --stage-target`.

During PS, the first snapshot is copied into anonymous memory owned by the
destination restore parent. The source then re-arms soft-dirty tracking and
re-reads every candidate locally. It compares each page with the first
snapshot in the same source MR, updating that MR and marking only changed
payloads. The target pageclient requests the first snapshot and the refreshed
metadata and changed payload runs over librdmacm rsockets into the *same*
target memfd. Four independent lanes reuse their connections across both PS
rounds; each request is at most 16 MiB. The restore parent copies
only changed selected pages into its existing anonymous resident mappings.
A marker is published only after every snapshot request completes; the restore parent
checks the snapshot nonce, page count and address/VMA identity before
applying it. PS and final IS validity manifests still decide which pages may
be adopted with `mremap`. Any page that cannot be recopied or fails final
validation falls back to the ordinary pageclient path.

The second round deliberately re-reads all candidate pages **locally**.
Dirty-only source reads based solely on soft-dirty could lose a write between
scanning the old epoch and clearing it, then adopt stale data. Comparing the
re-read bytes avoids that race while sending only changed pages over rsocket.
Writes after a page's re-read remain visible to final soft-dirty/PFN
validation. This baseline implements two resident snapshots; additional
rounds and source-side dirty-only reads remain separate work.

The 100k x 1 KiB smoke run `sb_ae_20260928_074739` passed all indexed key
lengths and the bytewise 8 MiB canary. Of 31,808 source candidates, 7,670
changed; the target read 32.94 MB including metadata and refreshed exactly
7,670 resident pages. Its client-wide success gap was 106.158 ms.

The 500k x 10 KiB run `sb_ae_20260928_074945` also passed all indexed key
lengths and canary checks. Of 1,533,250 source candidates, 267,631 changed.
The target read 73.60 MB of metadata and 1.096 GB of changed payloads,
then refreshed exactly 267,631 resident pages. Its client-wide success gap
was 328.775 ms; the stable target reference was 44.8k ops/s and TTR90 began
3.520 seconds after service resumed. The earlier full second-round run
`sb_ae_20260928_065735` remains archived for comparison: it re-read about
6.35 GB over RDMA and observed a 362.427 ms gap and 31.920 s TTR90 start.
Both are single runs, not a statistical speedup estimate. Indexed values
were checked for presence and length, not individually bytewise checksummed.
That older run used native ibverbs for the PS payload. The current rsocket
version keeps the U/AS page-fault and background paths on native ibverbs;
the PCLive PS snapshot payload is the part moved to rsocket so far.

The current full rsocket run `sb_ae_20260928_091321` passed all 500,000
indexed key lengths and the bytewise 8 MiB canary. Its first snapshot moved
6,358,683,648 bytes in 2.257 seconds over four persistent lanes. The second
round moved 73,654,272 bytes of metadata plus 992,985,088 bytes for 242,428
changed pages in 0.445 seconds, and exactly 242,428 resident pages were
refreshed. The client-wide success gap was 410.560 ms; the stable target rate
was 44,727 ops/s, and TTR90 began 2.430 seconds after service resumed.
These are one-run end-to-end observations under the 25 Gbps NIC limit.
