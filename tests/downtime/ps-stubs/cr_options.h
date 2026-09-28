/* Only unrelated CRIU option plumbing is stubbed; snapshot/pool code is real. */
struct options { unsigned sb_precopy_workers, sb_precopy_limit_mb, sb_kernel_ps_chunk_mb; };
extern struct options opts;
