// SPDX-License-Identifier: GPL-2.0
/* Unbound PTE tokens are session resources, not speculative application state.
 * Bind a cookie once, before publishing a PTE. Faults never acquire pool locks. */
#include <linux/module.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include "sbk_token_pool.h"
#ifdef CONFIG_SWIFTBATON_PTE
#define SBK_TOKEN_CHUNK 65536UL
/* Deterministic reservation-failure coverage in the isolated VM only. */
static long token_pool_test_fail_after = -1;
module_param(token_pool_test_fail_after, long, 0600);
MODULE_PARM_DESC(token_pool_test_fail_after, "Validation injection: fail after this many prepared tokens; -1 disables");
struct sbk_token_chunk;
struct sbk_token_slot {
	struct sbk_token_chunk *chunk;
	void *cookie;
	unsigned long id;
};
struct sbk_token_chunk {
	struct kref refs;
	struct work_struct destroy;
	struct workqueue_struct *reaper;
	struct list_head list;
	const struct sbk_pte_provider *ops;
	unsigned long count, next;
	struct sbk_token_slot slots[];
};
struct sbk_token_pool {
	struct kref refs;
	struct mutex lock;
	struct list_head chunks;
	const struct sbk_pte_provider *ops;
	struct workqueue_struct *reaper;
	unsigned long available, prepared, claimed;
	atomic64_t fallback;
	bool sealed;
};
static void chunk_destroy(struct work_struct *work)
{
	struct sbk_token_chunk *chunk = container_of(work, struct sbk_token_chunk, destroy);
	kvfree(chunk);
}
static void chunk_release(struct kref *ref)
{
	struct sbk_token_chunk *chunk = container_of(ref, struct sbk_token_chunk, refs);
	/* The last token can retire under a PTE spinlock. module_exit joins the
	 * owning module's reaper before unloading this callback. */
	queue_work(chunk->reaper, &chunk->destroy);
}
static struct page *slot_get(void *cookie, struct vm_area_struct *vma,
                            unsigned long address, unsigned int lane, bool user)
{
	struct sbk_token_slot *slot = cookie;
	return slot->chunk->ops->get_page(slot->cookie, vma, address, lane, user);
}
static void slot_installed(void *cookie, struct mm_struct *mm,
                           unsigned long address, unsigned int lane, bool user)
{
	struct sbk_token_slot *slot = cookie;
	if (slot->chunk->ops->installed)
		slot->chunk->ops->installed(slot->cookie, mm, address, lane, user);
}
static void slot_fault_done(void *cookie, u64 ns, vm_fault_t result)
{
	struct sbk_token_slot *slot = cookie;
	if (slot->chunk->ops->fault_done)
		slot->chunk->ops->fault_done(slot->cookie, ns, result);
}
static void slot_release(void *cookie)
{
	struct sbk_token_slot *slot = cookie;
	if (slot->cookie)
		slot->chunk->ops->release(slot->cookie);
	kref_put(&slot->chunk->refs, chunk_release);
}
static const struct sbk_pte_provider prepared_provider = {
	.owner = THIS_MODULE, .get_page = slot_get, .installed = slot_installed,
	.fault_done = slot_fault_done, .release = slot_release,
};
static void pool_release(struct kref *ref)
{
	struct sbk_token_pool *pool = container_of(ref, struct sbk_token_pool, refs);
	struct sbk_token_chunk *chunk, *next;
	/* Last pool holder: no concurrent reserve/take remains. Tokens already
	 * handed to regions retain their chunks independently of this list. */
	list_for_each_entry_safe(chunk, next, &pool->chunks, list) {
		unsigned long i;
		list_del(&chunk->list);
		for (i = chunk->next; i < chunk->count; i++)
			sbk_pte_token_put(chunk->slots[i].id);
		kref_put(&chunk->refs, chunk_release);
	}
	kfree(pool);
}
struct sbk_token_pool *sbk_token_pool_create(const struct sbk_pte_provider *ops,
                                           struct workqueue_struct *reaper)
{
	struct sbk_token_pool *pool = kzalloc(sizeof(*pool), GFP_KERNEL);
	if (!pool)
		return NULL;
	kref_init(&pool->refs);
	mutex_init(&pool->lock);
	INIT_LIST_HEAD(&pool->chunks);
	pool->ops = ops;
	pool->reaper = reaper;
	return pool;
}
void sbk_token_pool_get(struct sbk_token_pool *pool)
{
	kref_get(&pool->refs);
}
void sbk_token_pool_put(struct sbk_token_pool *pool)
{
	if (pool)
		kref_put(&pool->refs, pool_release);
}
int sbk_token_pool_reserve(struct sbk_token_pool *pool, unsigned long pages)
{
	int ret = 0;
	if (!pages || pages > SBK_TOKEN_POOL_MAX_PAGES)
		return -EINVAL;
	mutex_lock(&pool->lock);
	if (pool->sealed) {
		ret = -EBUSY;
		goto out;
	}
	while (pool->available < pages) {
		unsigned long i, n = min_t(unsigned long, pages - pool->available, SBK_TOKEN_CHUNK);
		struct sbk_token_chunk *chunk = kvzalloc(struct_size(chunk, slots, n), GFP_KERNEL);
		if (!chunk) {
			ret = -ENOMEM;
			break;
		}
		kref_init(&chunk->refs); /* Pool list owns one reference. */
		INIT_WORK(&chunk->destroy, chunk_destroy);
		chunk->reaper = pool->reaper;
		chunk->ops = pool->ops;
		for (i = 0; i < n; i++) {
			struct sbk_token_slot *slot = &chunk->slots[i];
			long fail_after = READ_ONCE(token_pool_test_fail_after);
			if (fail_after >= 0 && pool->prepared + chunk->count >= fail_after) {
				ret = -ENOMEM;
				break;
			}
			slot->chunk = chunk;
			kref_get(&chunk->refs);
			ret = sbk_pte_token_create(&prepared_provider, slot, &slot->id);
			if (ret) {
				kref_put(&chunk->refs, chunk_release);
				break;
			}
			chunk->count++;
			if (!(i & 1023))
				cond_resched();
		}
		if (chunk->count) {
			list_add_tail(&chunk->list, &pool->chunks);
			pool->available += chunk->count;
			pool->prepared += chunk->count;
		} else {
			kref_put(&chunk->refs, chunk_release);
		}
		if (ret)
			break; /* Partial preparation stays owned until close or take. */
	}
out:
	mutex_unlock(&pool->lock);
	return ret;
}
unsigned long sbk_token_pool_take(struct sbk_token_pool *pool, void *cookies,
                                 size_t stride, unsigned long pages,
                                 unsigned long *ids, void (*retain)(void *))
{
	unsigned long done = 0;
	mutex_lock(&pool->lock);
	pool->sealed = true;
	while (done < pages && !list_empty(&pool->chunks)) {
		struct sbk_token_chunk *chunk = list_first_entry(&pool->chunks, struct sbk_token_chunk, list);
		unsigned long n = min(pages - done, chunk->count - chunk->next), i;
		for (i = 0; i < n; i++, done++) {
			struct sbk_token_slot *slot = &chunk->slots[chunk->next++];
			void *cookie = (char *)cookies + done * stride;
			retain(cookie);
			slot->cookie = cookie; /* Immutable once a marker can be visible. */
			ids[done] = slot->id;
		}
		pool->available -= n;
		pool->claimed += n;
		if (chunk->next == chunk->count) {
			list_del(&chunk->list);
			kref_put(&chunk->refs, chunk_release);
		}
	}
	mutex_unlock(&pool->lock);
	return done;
}
void sbk_token_pool_fallback(struct sbk_token_pool *pool)
{
	atomic64_inc(&pool->fallback);
}
void sbk_token_pool_stats(struct sbk_token_pool *pool, struct sbk_token_pool_stats *stats)
{
	mutex_lock(&pool->lock);
	stats->available = pool->available;
	stats->prepared = pool->prepared;
	stats->claimed = pool->claimed;
	stats->fallback = atomic64_read(&pool->fallback);
	stats->sealed = pool->sealed;
	mutex_unlock(&pool->lock);
}
#endif
