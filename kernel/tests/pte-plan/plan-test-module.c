// SPDX-License-Identifier: GPL-2.0
/* Isolated guest fixture. Never loaded on the research hosts. */
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/swiftbaton_pte.h>
#include "plan-test.h"

static atomic64_t created, released, faults;
struct plan_file {
	struct mutex lock;
	struct sbk_pte_plan *plan;
	unsigned long *ids, pages, creators, address;
	bool attempted;
};
struct plan_cookie { unsigned long index; };
static struct page *plan_page(void *opaque, struct vm_area_struct *vma,
	unsigned long address, unsigned int lane, bool user_fault)
{
	struct plan_cookie *cookie = opaque;
	struct page *page = alloc_page(GFP_KERNEL);
	if (!page)
		return ERR_PTR(-ENOMEM);
	memset(page_address(page), (cookie->index * 37 + 11) & 255, PAGE_SIZE);
	atomic64_inc(&faults);
	return page;
}
static void plan_release(void *cookie)
{
	atomic64_inc(&released);
	kfree(cookie);
}
static const struct sbk_pte_provider provider = {
	.owner = THIS_MODULE, .get_page = plan_page, .release = plan_release,
};
static void drop_creators(struct plan_file *f)
{
	unsigned long i;
	for (i = 0; i < f->creators; i++) {
		sbk_pte_token_put(f->ids[i]);
		if (!(i & 4095))
			cond_resched();
	}
	f->creators = 0;
}
static int plan_open(struct inode *inode, struct file *file)
{
	struct plan_file *f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f)
		return -ENOMEM;
	mutex_init(&f->lock);
	file->private_data = f;
	return 0;
}
static int plan_close(struct inode *inode, struct file *file)
{
	struct plan_file *f = file->private_data;
	sbk_pte_plan_free(f->plan);
	drop_creators(f);
	kvfree(f->ids);
	kfree(f);
	return 0;
}
static long plan_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct plan_file *f = file->private_data;
	struct plan_request q;
	struct plan_result result = {};
	struct sbk_pte_plan_stats stats;
	u64 index;
	unsigned long i, saved = 0;
	int ret = 0;
	mutex_lock(&f->lock);
	switch (cmd) {
#ifdef CONFIG_SWIFTBATON_PTE_PLAN_TEST
	case PLAN_FLUSH:
		sbk_pte_plan_test_flush_memcg();
		break;
	case PLAN_INJECT:
		if (!f->plan) { ret = -EINVAL; break; }
		if (copy_from_user(&index, (void __user *)arg, sizeof(index))) { ret = -EFAULT; break; }
		ret = sbk_pte_plan_test_fail_after(f->plan, index);
		break;
#endif
	case PLAN_PREPARE:
		if (f->ids) { ret = -EALREADY; break; }
		if (copy_from_user(&q, (void __user *)arg, sizeof(q))) { ret = -EFAULT; break; }
		if (!q.pages || q.pages > (1UL << 20) || q.mode > 3) { ret = -EINVAL; break; }
		f->ids = kvcalloc(q.pages, sizeof(*f->ids), GFP_KERNEL);
		if (!f->ids) { ret = -ENOMEM; break; }
		f->pages = q.pages;
		f->address = q.address;
		for (i = 0; i < q.pages; i++) {
			struct plan_cookie *cookie = kmalloc(sizeof(*cookie), GFP_KERNEL);
			if (!cookie) { ret = -ENOMEM; break; }
			cookie->index = i;
			ret = sbk_pte_token_create(&provider, cookie, &f->ids[i]);
			if (ret) { kfree(cookie); break; }
			atomic64_inc(&created);
			f->creators++;
			if (!(i & 4095)) cond_resched();
		}
		if (!ret && q.mode != 3) {
			/* Restore malformed test ID before dropping original creators. */
			saved = f->ids[q.pages - 1];
			if (q.mode == 1) f->ids[q.pages - 1] = f->ids[0];
			if (q.mode == 2) f->ids[q.pages - 1] = ULONG_MAX;
			f->plan = sbk_pte_plan_create(q.address, q.pages, f->ids);
			f->ids[q.pages - 1] = saved;
			if (IS_ERR(f->plan)) { ret = PTR_ERR(f->plan); f->plan = NULL; }
		}
		if (ret || q.mode != 3) drop_creators(f);
		break;
	case PLAN_ARM:
		if (!f->ids || (!f->plan && !f->creators)) { ret = -EINVAL; break; }
		if (f->attempted) { ret = -EALREADY; break; }
		f->attempted = true;
		if (f->plan) ret = sbk_pte_plan_arm(f->plan, current->mm);
		else {
			ret = sbk_pte_arm(current->mm, f->address, f->pages, f->ids);
			drop_creators(f);
		}
		break;
	case PLAN_POPULATE:
		if (copy_from_user(&index, (void __user *)arg, sizeof(index))) { ret = -EFAULT; break; }
		if (index >= f->pages) { ret = -EINVAL; break; }
		ret = sbk_pte_populate_all(f->ids[index], SBK_PTE_BACKGROUND);
		break;
	case PLAN_STAT:
		result.created = atomic64_read(&created);
		result.released = atomic64_read(&released);
		result.faults = atomic64_read(&faults);
		if (f->plan) {
			sbk_pte_plan_stats(f->plan, &stats);
			result.pages = stats.pages;
			result.tables = stats.tables;
			result.published = stats.published;
			result.moved_tables = stats.moved_tables;
			result.copied_pages = stats.copied_pages;
		}
		if (copy_to_user((void __user *)arg, &result, sizeof(result))) ret = -EFAULT;
		break;
	default: ret = -ENOTTY;
	}
	mutex_unlock(&f->lock);
	return ret;
}
static const struct file_operations operations = {
	.owner = THIS_MODULE, .open = plan_open, .release = plan_close,
	.unlocked_ioctl = plan_ioctl, .llseek = no_llseek,
};
static struct miscdevice device = {
	.minor = MISC_DYNAMIC_MINOR, .name = "sbk_plan_test", .fops = &operations, .mode = 0600,
};
static int __init plan_init(void) { return misc_register(&device); }
static void __exit plan_exit(void) { misc_deregister(&device); }
module_init(plan_init);
module_exit(plan_exit);
MODULE_LICENSE("GPL");
