#ifndef __CR_KCMP_IDS_H__
#define __CR_KCMP_IDS_H__

#include <stdint.h>
#include <pthread.h>
#include <sys/types.h>

#include "kcmp.h"

struct kid_tree {
	pthread_mutex_t lock;
	struct rb_root root;
	unsigned int kcmp_type;
	unsigned long subid;
};

#define DECLARE_KCMP_TREE(name, type) \
	struct kid_tree name = {      \
		.lock = PTHREAD_MUTEX_INITIALIZER, \
		.root = RB_ROOT,      \
		.kcmp_type = type,    \
		.subid = 1,           \
	}

struct kid_elem {
	pid_t pid;
	unsigned int genid;
	unsigned int idx;
};

/* Allocate from the same ID namespace as tree nodes, under its lock. */
extern uint32_t kid_generate_id(struct kid_tree *tree);

extern uint32_t kid_generate_gen(struct kid_tree *tree, struct kid_elem *elem, int *new_id);

extern struct kid_elem *kid_lookup_epoll_tfd(struct kid_tree *tree, struct kid_elem *elem, kcmp_epoll_slot_t *slot);

#endif /* __CR_KCMP_IDS_H__ */
