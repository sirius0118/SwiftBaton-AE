#include <stdio.h>
#include <string.h>
#include <errno.h>
#define pr_info(...) fprintf(stderr, __VA_ARGS__)
#define pr_err(...) fprintf(stderr, __VA_ARGS__)
#define pr_perror(fmt, ...) fprintf(stderr, fmt ": %s\n", ##__VA_ARGS__, strerror(errno))
