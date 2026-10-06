#define _GNU_SOURCE
#include "sb-cpu-limit.h"
#include <pthread.h>
#include <sys/wait.h>

static cpu_set_t initial;
static void *check_thread(void *unused)
{
	cpu_set_t mask;
	(void)unused;
	if (sched_getaffinity(0, sizeof(mask), &mask) || !CPU_EQUAL(&mask, &initial))
		return (void *)1;
	return NULL;
}

int main(void)
{
	pthread_t threads[8];
	void *result;
	pid_t child;
	int i, status;
	if (sb_cpu_limit() || sched_getaffinity(0, sizeof(initial), &initial)) return 1;
	for (i = 0; i < 8; i++)
		if (pthread_create(&threads[i], NULL, check_thread, NULL)) return 2;
	for (i = 0; i < 8; i++)
		if (pthread_join(threads[i], &result) || result) return 3;
	child = fork();
	if (child < 0) return 4;
	if (!child) _exit(check_thread(NULL) ? 1 : 0);
	if (waitpid(child, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status)) return 5;
	printf("CPU_LIMIT_INHERITANCE_PASS cpus=%d\n", CPU_COUNT(&initial));
	return 0;
}
