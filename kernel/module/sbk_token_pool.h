/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SBK_TOKEN_POOL_H
#define SBK_TOKEN_POOL_H
#include <linux/types.h>
struct sbk_token_pool;
#ifdef CONFIG_SWIFTBATON_PTE
#include <linux/swiftbaton_pte.h>
#include "sbk_uapi.h"
struct workqueue_struct;
struct sbk_token_pool *sbk_token_pool_create(const struct sbk_pte_provider *ops,
                                           struct workqueue_struct *reaper);
void sbk_token_pool_get(struct sbk_token_pool *pool);
void sbk_token_pool_put(struct sbk_token_pool *pool);
int sbk_token_pool_reserve(struct sbk_token_pool *pool, unsigned long pages);
void sbk_token_pool_stats(struct sbk_token_pool *pool, struct sbk_token_pool_stats *stats);
unsigned long sbk_token_pool_take(struct sbk_token_pool *pool, void *cookies,
                                 size_t stride, unsigned long pages,
                                 unsigned long *ids, void (*retain)(void *));
void sbk_token_pool_fallback(struct sbk_token_pool *pool);
#endif
#endif
