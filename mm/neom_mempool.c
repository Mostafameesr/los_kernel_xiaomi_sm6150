// SPDX-License-Identifier: GPL-2.0
/*
 * Bounded order-0 reserve for init-registered native processes on non-NUMA
 * HyperOS ports. Follows Xiaomi's reserve-before-direct-reclaim design, with
 * ordinary 4.14 allocator and shrinker integration instead of GKI hooks.
 */
#include <linux/cpuset.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kobject.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/neom_reserve.h>
#include <linux/shrinker.h>
#include <linux/sizes.h>
#include <linux/swap.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#define MI_POOL_PAGES ((32UL * SZ_1M) >> PAGE_SHIFT)
#define MI_POOL_BATCH 256
#define MI_POOL_HEADROOM ((96UL * SZ_1M) >> PAGE_SHIFT)

static struct neom_pid_list mi_pids = NEOM_PID_LIST_INIT(mi_pids);
static DEFINE_SPINLOCK(mi_pool_lock);
static DEFINE_MUTEX(mi_pool_control);
static LIST_HEAD(mi_pool_pages);
static unsigned long mi_pool_count;
static unsigned long mi_pool_pause;
static bool mi_pool_ready;
static bool mi_pool_enabled = true;
static atomic_long_t mi_pool_hits = ATOMIC_LONG_INIT(0);
static atomic_long_t mi_pool_misses = ATOMIC_LONG_INIT(0);
static struct delayed_work mi_pool_work;

static struct page *mi_pool_remove(void)
{
	struct page *page = NULL;
	unsigned long flags;

	spin_lock_irqsave(&mi_pool_lock, flags);
	if (!list_empty(&mi_pool_pages)) {
		page = list_first_entry(&mi_pool_pages, struct page, lru);
		list_del_init(&page->lru);
		mi_pool_count--;
		mod_node_page_state(page_pgdat(page), NR_KERNEL_MISC_RECLAIMABLE,
				    -1);
	}
	spin_unlock_irqrestore(&mi_pool_lock, flags);
	return page;
}

static unsigned long mi_pool_drain(unsigned long nr)
{
	unsigned long freed = 0;
	struct page *page;

	while (freed < nr && (page = mi_pool_remove())) {
		__free_page(page);
		freed++;
	}
	return freed;
}

static void mi_pool_refill(struct work_struct *work)
{
	unsigned long flags;
	unsigned long delay = HZ;
	int i;

	if (!READ_ONCE(mi_pool_enabled) || !neom_pid_list_has_live(&mi_pids)) {
		mi_pool_drain(ULONG_MAX);
		return;
	}
	if (time_before(jiffies, READ_ONCE(mi_pool_pause)))
		goto reschedule;
	for (i = 0; i < MI_POOL_BATCH; i++) {
		struct page *page;
		bool added = false;

		if (READ_ONCE(mi_pool_count) >= MI_POOL_PAGES ||
		    si_mem_available() < (long)MI_POOL_HEADROOM)
			break;
		/* Never enter direct reclaim from a reserve refill worker. */
		page = alloc_page((GFP_KERNEL | __GFP_ZERO | __GFP_NOWARN |
				   __GFP_NORETRY) & ~__GFP_DIRECT_RECLAIM);
		if (!page)
			break;
		spin_lock_irqsave(&mi_pool_lock, flags);
		if (mi_pool_enabled && mi_pool_count < MI_POOL_PAGES &&
		    !time_before(jiffies, mi_pool_pause)) {
			list_add_tail(&page->lru, &mi_pool_pages);
			mi_pool_count++;
			mod_node_page_state(page_pgdat(page),
					    NR_KERNEL_MISC_RECLAIMABLE, 1);
			added = true;
		}
		spin_unlock_irqrestore(&mi_pool_lock, flags);
		if (!added) {
			__free_page(page);
			break;
		}
		cond_resched();
	}
	if (i == MI_POOL_BATCH && READ_ONCE(mi_pool_count) < MI_POOL_PAGES)
		delay = msecs_to_jiffies(20);
reschedule:
	if (READ_ONCE(mi_pool_enabled))
		queue_delayed_work(system_freezable_wq, &mi_pool_work, delay);
}

struct page *neom_mempool_alloc(gfp_t gfp_mask, unsigned int order,
				const nodemask_t *nodemask)
{
	struct page *page;

	if (!READ_ONCE(mi_pool_ready) || !READ_ONCE(mi_pool_enabled) || order ||
	    nodemask || (gfp_mask & GFP_KERNEL) != GFP_KERNEL ||
	    gfp_zone(gfp_mask) < ZONE_NORMAL ||
	    !neom_pid_list_contains(&mi_pids))
		return NULL;
	page = mi_pool_remove();
	if (page && !cpuset_zone_allowed(page_zone(page), gfp_mask)) {
		__free_page(page);
		page = NULL;
	}
	if (page)
		atomic_long_inc(&mi_pool_hits);
	else
		atomic_long_inc(&mi_pool_misses);
	return page;
}

static unsigned long mi_pool_count_objects(struct shrinker *shrinker,
					   struct shrink_control *sc)
{
	return READ_ONCE(mi_pool_count);
}

static unsigned long mi_pool_scan_objects(struct shrinker *shrinker,
					  struct shrink_control *sc)
{
	unsigned long flags;

	spin_lock_irqsave(&mi_pool_lock, flags);
	mi_pool_pause = jiffies + HZ;
	spin_unlock_irqrestore(&mi_pool_lock, flags);
	return mi_pool_drain(sc->nr_to_scan);
}

static struct shrinker mi_pool_shrinker = {
	.count_objects = mi_pool_count_objects,
	.scan_objects = mi_pool_scan_objects,
	.seeks = DEFAULT_SEEKS,
};

static ssize_t pids_show(struct kobject *kobj, struct kobj_attribute *attr,
			 char *buf)
{
	return neom_pid_list_show(&mi_pids, buf);
}

static ssize_t pids_store(struct kobject *kobj, struct kobj_attribute *attr,
			  const char *buf, size_t count)
{
	ssize_t ret = neom_pid_list_store(&mi_pids, buf, count);

	if (ret > 0 && READ_ONCE(mi_pool_enabled))
		mod_delayed_work(system_freezable_wq, &mi_pool_work, 0);
	return ret;
}

static ssize_t stats_show(struct kobject *kobj, struct kobj_attribute *attr,
			  char *buf)
{
	return scnprintf(buf, PAGE_SIZE,
		"pages=%lu limit_pages=%lu hits=%ld misses=%ld order=0\n",
		READ_ONCE(mi_pool_count), MI_POOL_PAGES,
		atomic_long_read(&mi_pool_hits), atomic_long_read(&mi_pool_misses));
}

static ssize_t enable_show(struct kobject *kobj, struct kobj_attribute *attr,
			   char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", READ_ONCE(mi_pool_enabled));
}

static ssize_t enable_store(struct kobject *kobj, struct kobj_attribute *attr,
			    const char *buf, size_t count)
{
	bool enabled;
	int ret = kstrtobool(buf, &enabled);

	if (ret)
		return ret;
	mutex_lock(&mi_pool_control);
	WRITE_ONCE(mi_pool_enabled, enabled);
	if (enabled) {
		mod_delayed_work(system_freezable_wq, &mi_pool_work, 0);
	} else {
		cancel_delayed_work_sync(&mi_pool_work);
		mi_pool_drain(ULONG_MAX);
	}
	mutex_unlock(&mi_pool_control);
	return count;
}

static struct kobj_attribute pids_attr = __ATTR(pids, 0660, pids_show, pids_store);
static struct kobj_attribute stats_attr = __ATTR_RO(stats);
static struct kobj_attribute enable_attr = __ATTR_RW(enable);
static struct attribute *mi_pool_attrs[] = {
	&pids_attr.attr, &stats_attr.attr, &enable_attr.attr, NULL,
};

static const struct attribute_group mi_pool_group = { .attrs = mi_pool_attrs };

static int __init neom_mempool_init(void)
{
	struct kobject *kobj;
	int ret;

	INIT_DELAYED_WORK(&mi_pool_work, mi_pool_refill);
	mi_pool_pause = jiffies;
	ret = register_shrinker(&mi_pool_shrinker);
	if (ret)
		return ret;
	kobj = kobject_create_and_add("mi_mempool", kernel_kobj);
	if (!kobj) {
		ret = -ENOMEM;
		goto unregister;
	}
	ret = sysfs_create_group(kobj, &mi_pool_group);
	if (ret) {
		kobject_put(kobj);
		goto unregister;
	}
	WRITE_ONCE(mi_pool_ready, true);
	pr_info("neom: registered shrinkable order-0 mi_mempool reserve\n");
	return 0;
unregister:
	unregister_shrinker(&mi_pool_shrinker);
	return ret;
}
late_initcall(neom_mempool_init);
