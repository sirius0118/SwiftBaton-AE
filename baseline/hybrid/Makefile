CC ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Werror

fragmentation_stress: fragmentation_stress.c ../../criu/criu/sb-sched.c ../../criu/criu/include/sb-sched.h
	$(CC) $(CFLAGS) -iquote ../../criu/criu/include -o $@ fragmentation_stress.c ../../criu/criu/sb-sched.c

.PHONY: clean
clean:
	rm -f fragmentation_stress
