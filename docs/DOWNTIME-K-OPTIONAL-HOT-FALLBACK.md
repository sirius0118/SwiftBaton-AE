# K downtime: optional hot hint for ranges absent from PS

This experimental branch leaves the hot-order hint empty for a final range
that neither matches nor fits within a PS layout range. The K demand and
background transfer paths still cover the range. The old fallback rescanned
the complete sampled heat list while the source was frozen; prior runs with
three such ranges spent about 2.8 ms in that step.

One full 500k-record Redis run (`sb_ae_20260928_034802`) passed migration,
all-record presence/length, 8 MiB bytewise canary, module error counter,
recovery analysis and host rollback checks. Client gap was 58.47 ms, source
final prepare 11.06 ms, hot handling 0.08 ms, and TTR90 2.39 s. This run
had zero unmatched ranges: it verifies the common path but does **not**
measure the proposed fallback omission. The lower client gap cannot be
attributed to this change. Further trials with unmatched ranges are needed
before promotion.
