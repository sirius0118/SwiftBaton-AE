#include <errno.h>
#include <stdlib.h>
#include <sys/mman.h>
#include "sb-fork.h"

struct wipe_range {
    void *address;
    size_t length;
    struct wipe_range *next;
};
static struct wipe_range *ranges;

int sb_fork_wipe_private(void *address, size_t length)
{
    struct wipe_range *r;
    if (!address || !length) { errno = EINVAL; return -1; }
    r = malloc(sizeof(*r));
    if (!r) return -1;
    if (madvise(address, length, MADV_WIPEONFORK)) { free(r); return -1; }
    *r = (struct wipe_range){ .address = address, .length = length, .next = ranges };
    ranges = r;
    return 0;
}

int sb_fork_restore_inheritance(void)
{
    int saved_errno = 0;
    while (ranges) {
        struct wipe_range *r = ranges;
        if (madvise(r->address, r->length, MADV_KEEPONFORK) && !saved_errno)
            saved_errno = errno;
        ranges = r->next;
        free(r);
    }
    if (saved_errno) { errno = saved_errno; return -1; }
    return 0;
}
