#ifndef __SB_HEAT_H__
#define __SB_HEAT_H__
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
/* Scheduling hints only. Reuse still requires the separate final validator. */
struct sb_heat_page {
    pid_t pid;
    unsigned accesses, writes, observations;
    uint64_t address;
};
int sb_heat_collect(const pid_t *pids, size_t count, unsigned rounds, unsigned interval_us,
                    struct sb_heat_page **pages, size_t *nr_pages);
#endif
