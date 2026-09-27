/* SPDX-License-Identifier: GPL-2.0 */
/* Optional PS control path. Included after the anonymous token provider.
 * Normal ARM remains available on kernels without the detached-plan API. */
static int sbk_prepare_anonymous(struct sbk_context *c, void __user *user)
{
	struct sbk_anon_arm a;
	struct sbk_pte_plan *plan;
	unsigned long i;
	int ret;
	u64 began = arm_timing ? ktime_get_ns() : 0;
	if (!c->configured || c->mapped || c->plan_mode || c->token_ids ||
	    c->tokens_created || atomic_read(&c->stopping))
		return -EINVAL;
	if (copy_from_user(&a, user, sizeof(a)))
		return -EFAULT;
	if ((a.address & ~PAGE_MASK) || a.address > ULONG_MAX - (c->cfg.pages << PAGE_SHIFT))
		return -EINVAL;
	/* Once token binding begins, a failure requires closing this region and
	 * preparing a fresh one. Reserved IDs must never be recycled through ARM. */
	c->plan_mode = true;
	c->plan_address = a.address;
	atomic64_set(&c->retired_tokens, 0);
	c->token_ids = kvcalloc(c->cfg.pages, sizeof(*c->token_ids), GFP_KERNEL);
	if (!c->token_ids) {
		ret = -ENOMEM;
		goto fail;
	}
	c->tokens_created = sbk_token_pool_take(c->token_pool, c->entries,
		sizeof(*c->entries), c->cfg.pages, c->token_ids, sbk_token_retain);
	for (i = c->tokens_created; i < c->cfg.pages; i++) {
		kref_get(&c->refs);
		ret = sbk_pte_token_create(&sbk_anon_provider, &c->entries[i], &c->token_ids[i]);
		if (ret) {
			kref_put(&c->refs, sbk_release_ref);
			goto fail;
		}
		c->tokens_created++;
		sbk_token_pool_fallback(c->token_pool);
	}
	plan = sbk_pte_plan_create(a.address, c->cfg.pages, c->token_ids);
	if (IS_ERR(plan)) {
		ret = PTR_ERR(plan);
		goto fail;
	}
	c->arm_plan = plan;
	/* The detached plan now owns every token. Pay the creator ref cost in
	 * PS on this caller, without a later worker competing with demand faults. */
	for (i = 0; i < c->tokens_created; i++) {
		sbk_pte_token_put(c->token_ids[i]);
		if (!(i & 4095))
			cond_resched();
	}
	c->tokens_created = 0;
	if (arm_timing)
		pr_info("SBK_PREPARE_ARM pages=%llu total_ns=%llu\n", c->cfg.pages, ktime_get_ns() - began);
	return 0;
fail:
	for (i = 0; i < c->tokens_created; i++)
		sbk_pte_token_put(c->token_ids[i]);
	c->tokens_created = 0;
	kvfree(c->token_ids);
	c->token_ids = NULL;
	atomic_set(&c->stopping, 1);
	return ret;
}

static int sbk_arm_prepared(struct sbk_context *c, const struct sbk_anon_arm *a)
{
	u64 began = arm_timing ? ktime_get_ns() : 0;
	int ret;
	/* The caller's final layout/presence and PS-byte validation still apply.
	 * The old ARM guard already requires SEAL after any speculative import. */
	if (a->address != c->plan_address)
		return -ESTALE;
	c->base = a->address;
	c->mm = current->mm;
	mmgrab(c->mm);
	c->anonymous = c->mapped = true;
	ret = sbk_pte_plan_arm(c->arm_plan, c->mm);
	if (ret) {
		c->anonymous = c->mapped = false;
		mmdrop(c->mm);
		c->mm = NULL;
		/* Partial rollback may already have retired some entries. A fresh
		 * context is required; prevent retry/background on this failed one. */
		atomic_set(&c->stopping, 1);
		sbk_pte_plan_free(c->arm_plan);
		c->arm_plan = NULL;
		return ret;
	}
	c->sealed = true;
	atomic_set_release(&c->armed, 1);
	sbk_maybe_drain(c);
	if (arm_timing)
		pr_info("SBK_ARM_PREPARED pages=%llu total_ns=%llu\n", c->cfg.pages, ktime_get_ns() - began);
	return 0;
}
