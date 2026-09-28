#ifndef SB_VMA_JOURNAL_H
#define SB_VMA_JOURNAL_H
#define SB_VMA_JOURNAL_CAPACITY 256
/* Immutable after syscall exit. Readers require active=0, !poisoned and an
 * unchanged generation around a complete snapshot. Overflow never means clean. */
struct journal_epoch { unsigned long long generation, active, poisoned, events; };
struct journal_key { unsigned long long sequence; unsigned cpu,reserved; };
struct journal_event {
    struct journal_key key;
    unsigned long long pid_tgid, syscall_nr, global_scope;
    unsigned long long args[6];
    long long result;
};
#endif
