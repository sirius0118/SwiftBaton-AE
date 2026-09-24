#ifndef __SB_PRECOPY_H__
#define __SB_PRECOPY_H__
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct sb_precopy_page { uint32_t pid, copied; uint64_t address, pfn, region_start, region_end, region_flags; };
struct sb_precopy_view {
    const struct sb_precopy_page *pages;
    const void *data;
    uint64_t count, nonce;
};
struct sb_precopy_pid { int32_t source, destination; };
typedef int (*sb_precopy_eligible_fn)(pid_t pid, uint64_t address);
typedef int (*sb_precopy_install_fn)(pid_t pid, uint64_t address, const void *bytes);
typedef int (*sb_precopy_adopted_fn)(pid_t pid, uint64_t index);

/* Diagnostic counters only; never changes the page-validity decision. */
void sb_precopy_trace_reasons(int enabled);

int sb_precopy_view(void *buffer, uint64_t length, struct sb_precopy_view *view);
int sb_precopy_validity(const struct sb_precopy_view *view, int dirfd,
                       struct sb_precopy_pid **pids, size_t *count, unsigned char **bitmap);
/* Conservative early rejection while the source is running. IS still checks
 * every accepted page, and may never reaccept a PS-rejected page. */
int sb_precopy_prune(int image_dir_fd);
int sb_precopy_ps_validity(const struct sb_precopy_view *view, int dirfd, unsigned char **bitmap);
void sb_precopy_set_adopted(sb_precopy_adopted_fn callback);

int sb_precopy_build(struct sb_precopy_page *candidates, size_t count,
                     uint64_t limit_bytes, unsigned workers, void **buffer, uint64_t *length);
/* Call with original application threads stopped. eligible() also reserves
 * accepted pages in the source background-transfer bitmap. */
int sb_precopy_finalize(const struct sb_precopy_pid *pids, size_t count,
                        int image_dir_fd, sb_precopy_eligible_fn eligible);
/* Parallel final validation shards even a single large process. eligible must
 * support concurrent calls, including reservations in the same bitmap word. */
int sb_precopy_finalize_workers(const struct sb_precopy_pid *pids, size_t count,
                               int image_dir_fd, sb_precopy_eligible_fn eligible, unsigned workers);
/* PS: validate the immutable received snapshot and preallocate its index.
 * Neither PID translation nor page reuse is authorized until client_init()
 * reads the final IS manifest. The buffer must remain immutable and alive. */
int sb_precopy_client_prepare(void *buffer, uint64_t length);
int sb_precopy_client_init(void *buffer, uint64_t length, int image_dir_fd,
                           sb_precopy_install_fn install);
/* Immutable final-validity lookup; does not copy or claim a page. */
int sb_precopy_client_has_page(pid_t pid, uint64_t address);
int sb_precopy_client_fault(pid_t pid, uint64_t address);
int sb_precopy_client_start(unsigned workers);
int sb_precopy_client_wait(void);
size_t sb_precopy_client_valid_pages(void);
#endif
