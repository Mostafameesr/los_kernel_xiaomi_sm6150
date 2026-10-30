// SPDX-License-Identifier: GPL-2.0
/*
 * ION adaptation of Xiaomi's PID-selected qcom system-heap reserve design.
 * Private cached/uncached pools are bounded and included in heap reclaim.
 * Secure and force-allocated buffers retain the original allocator paths.
 */
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/jiffies.h>
#include <linux/kobject.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/neom_reserve.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#include "ion_system_heap.h"
#include "ion_reserve_pool.h"

#define RESERVE_REFILL_BATCH 128
#define RESERVE_HEADROOM ((96UL * SZ_1M) >> PAGE_SHIFT)

struct ion_reserve_pool {
	struct ion_system_heap *heap;
	struct device *dev;
	struct ion_page_pool *pools[2][NUM_ORDERS];
	struct neom_pid_list pids;
	/* Serializes pool refill, free-to-reserve and pressure draining. */
	struct mutex state_lock;
	/* Serializes enable toggles while cancellation waits for work. */
	struct mutex control_lock;
	struct delayed_work refill;
	unsigned long pause_until;
	bool enabled;
	bool active;
	bool ready;
	atomic_long_t hits;
};

/* This ION implementation has built-in, boot-lifetime system heaps. */
static struct ion_reserve_pool *reserve_owner;
static DEFINE_MUTEX(reserve_owner_lock);

static unsigned int reserve_limit(unsigned int order)
{
	unsigned long bytes = order == 4 ? 8UL * SZ_1M : 4UL * SZ_1M;

	return bytes >> (PAGE_SHIFT + order);
}

static int reserve_count(struct ion_reserve_pool *reserve, gfp_t gfp_mask)
{
	int cached, i, count = 0;

	for (cached = 0; cached < 2; cached++)
		for (i = 0; i < NUM_ORDERS; i++)
			count += ion_page_pool_shrink(reserve->pools[cached][i],
						    gfp_mask, 0);
	return count;
}

/* Caller owns state_lock; a positive scan never becomes a count query. */
static int reserve_drain(struct ion_reserve_pool *reserve, gfp_t gfp_mask,
			 int nr_to_scan)
{
	int cached, i, freed = 0;

	for (cached = 0; cached < 2; cached++)
		for (i = 0; i < NUM_ORDERS && freed < nr_to_scan; i++)
			freed += ion_page_pool_shrink(reserve->pools[cached][i],
						     gfp_mask, nr_to_scan - freed);
	return freed;
}

static void reserve_refill(struct work_struct *work)
{
	struct ion_reserve_pool *reserve = container_of(to_delayed_work(work),
						struct ion_reserve_pool, refill);
	unsigned long delay = HZ;
	int cached, i, budget = RESERVE_REFILL_BATCH;

	mutex_lock(&reserve->state_lock);
	reserve->active = neom_pid_list_has_live(&reserve->pids);
	if (!reserve->enabled || !reserve->active) {
		reserve_drain(reserve, GFP_HIGHUSER, INT_MAX);
		mutex_unlock(&reserve->state_lock);
		return;
	}
	if (time_before(jiffies, reserve->pause_until))
		goto unlock;
	for (cached = 0; cached < 2; cached++) {
		for (i = 0; i < NUM_ORDERS; i++) {
			struct ion_page_pool *pool = reserve->pools[cached][i];
			unsigned int limit = reserve_limit(pool->order);

			while (READ_ONCE(pool->low_count) +
			       READ_ONCE(pool->high_count) < limit) {
				struct page *page;
				gfp_t gfp;

				if (!budget-- || si_mem_available() < (long)RESERVE_HEADROOM)
					goto unlock;
				gfp = (GFP_HIGHUSER | __GFP_ZERO | __GFP_NOWARN |
				       __GFP_NORETRY) & ~__GFP_DIRECT_RECLAIM;
				page = alloc_pages(gfp, pool->order);
				if (!page)
					goto unlock;
				ion_pages_sync_for_device(reserve->dev,
							  page, PAGE_SIZE << pool->order,
					DMA_BIDIRECTIONAL);
				if (!ion_page_pool_free_bounded(pool, page, limit))
					__free_pages(page, pool->order);
				cond_resched();
			}
		}
	}
unlock:
	if (budget < 0)
		delay = msecs_to_jiffies(20);
	mutex_unlock(&reserve->state_lock);
	if (READ_ONCE(reserve->enabled))
		queue_delayed_work(system_freezable_wq, &reserve->refill, delay);
}

struct page *ion_reserve_pool_alloc(struct ion_system_heap *heap,
				    bool cached, unsigned int index)
{
	/* Pairs with publication after all private pools are initialized. */
	struct ion_reserve_pool *reserve = smp_load_acquire(&heap->reserve);
	struct page *page;

	if (!reserve || !READ_ONCE(reserve->enabled) ||
	    !READ_ONCE(reserve->active) ||
	    !neom_pid_list_contains(&reserve->pids))
		return NULL;
	page = ion_page_pool_alloc_pool_only(reserve->pools[cached][index]);
	if (IS_ERR(page))
		return NULL;
	atomic_long_inc(&reserve->hits);
	return page;
}

bool ion_reserve_pool_free(struct ion_system_heap *heap, struct page *page,
			   bool cached, unsigned int index)
{
	/* Pairs with publication after all private pools are initialized. */
	struct ion_reserve_pool *reserve = smp_load_acquire(&heap->reserve);
	struct ion_page_pool *pool;
	bool added = false;

	if (!reserve || !READ_ONCE(reserve->enabled) ||
	    !READ_ONCE(reserve->active))
		return false;
	pool = reserve->pools[cached][index];
	if (READ_ONCE(pool->high_count) + READ_ONCE(pool->low_count) >=
	    reserve_limit(pool->order))
		return false;
	if (!mutex_trylock(&reserve->state_lock))
		return false;
	if (reserve->enabled && !time_before(jiffies, reserve->pause_until) &&
	    reserve->active)
		added = ion_page_pool_free_bounded(pool, page,
						   reserve_limit(pool->order));
	mutex_unlock(&reserve->state_lock);
	return added;
}

int ion_reserve_pool_shrink(struct ion_system_heap *heap, gfp_t gfp_mask,
			    int nr_to_scan)
{
	/* Pairs with publication after all private pools are initialized. */
	struct ion_reserve_pool *reserve = smp_load_acquire(&heap->reserve);
	int freed;

	if (!reserve)
		return 0;
	if (!nr_to_scan)
		return reserve_count(reserve, gfp_mask);
	if (!mutex_trylock(&reserve->state_lock))
		return 0;
	reserve->pause_until = jiffies + HZ;
	freed = reserve_drain(reserve, gfp_mask, nr_to_scan);
	mutex_unlock(&reserve->state_lock);
	return freed;
}

static ssize_t pid_show(struct kobject *kobj, struct kobj_attribute *attr,
			char *buf)
{
	return neom_pid_list_show(&reserve_owner->pids, buf);
}

static ssize_t pid_store(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t count)
{
	struct ion_reserve_pool *reserve = reserve_owner;
	ssize_t ret;

	if (!READ_ONCE(reserve->ready))
		return -EAGAIN;
	ret = neom_pid_list_store(&reserve->pids, buf, count);
	mutex_lock(&reserve->state_lock);
	reserve->active = neom_pid_list_has_live(&reserve->pids);
	mutex_unlock(&reserve->state_lock);

	if (ret > 0 && READ_ONCE(reserve->enabled))
		mod_delayed_work(system_freezable_wq, &reserve->refill, 0);
	return ret;
}

static ssize_t stats_show(struct kobject *kobj, struct kobj_attribute *attr,
			  char *buf)
{
	struct ion_reserve_pool *reserve = reserve_owner;

	return scnprintf(buf, PAGE_SIZE, "pages=%d hits=%ld secure_pages=0\n",
		reserve_count(reserve, GFP_HIGHUSER),
		atomic_long_read(&reserve->hits));
}

static ssize_t enable_show(struct kobject *kobj, struct kobj_attribute *attr,
			   char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", READ_ONCE(reserve_owner->enabled));
}

static ssize_t enable_store(struct kobject *kobj, struct kobj_attribute *attr,
			    const char *buf, size_t count)
{
	struct ion_reserve_pool *reserve = reserve_owner;
	bool enabled;
	int ret = kstrtobool(buf, &enabled);

	if (!READ_ONCE(reserve->ready))
		return -EAGAIN;
	if (ret)
		return ret;
	mutex_lock(&reserve->control_lock);
	mutex_lock(&reserve->state_lock);
	WRITE_ONCE(reserve->enabled, enabled);
	mutex_unlock(&reserve->state_lock);
	if (enabled) {
		mod_delayed_work(system_freezable_wq, &reserve->refill, 0);
	} else {
		cancel_delayed_work_sync(&reserve->refill);
		mutex_lock(&reserve->state_lock);
		reserve_drain(reserve, GFP_HIGHUSER, INT_MAX);
		mutex_unlock(&reserve->state_lock);
	}
	mutex_unlock(&reserve->control_lock);
	return count;
}

static struct kobj_attribute pid_attr = __ATTR(pid, 0660, pid_show, pid_store);
static struct kobj_attribute stats_attr = __ATTR_RO(stats);
static struct kobj_attribute enable_attr = __ATTR_RW(enable);
static struct attribute *reserve_attrs[] = {
	&pid_attr.attr, &stats_attr.attr, &enable_attr.attr, NULL,
};

static const struct attribute_group reserve_group = { .attrs = reserve_attrs };

void ion_reserve_pool_init(struct ion_system_heap *heap, struct device *dev)
{
	struct ion_reserve_pool *reserve;
	struct kobject *kobj;
	int cached, i, ret;

	/* Match the donor's >6 GiB guard; units here are 4.14 base pages. */
	if (totalram_pages <= ((6ULL * SZ_1G) >> PAGE_SHIFT))
		return;
	mutex_lock(&reserve_owner_lock);
	if (reserve_owner)
		goto unlock;
	reserve = kzalloc(sizeof(*reserve), GFP_KERNEL);
	if (!reserve)
		goto unlock;
	spin_lock_init(&reserve->pids.lock);
	mutex_init(&reserve->state_lock);
	mutex_init(&reserve->control_lock);
	INIT_DELAYED_WORK(&reserve->refill, reserve_refill);
	reserve->heap = heap;
	reserve->dev = dev;
	reserve->enabled = true;
	reserve->pause_until = jiffies;
	for (cached = 0; cached < 2; cached++) {
		for (i = 0; i < NUM_ORDERS; i++) {
			struct ion_page_pool *pool;

			pool = ion_page_pool_create(GFP_HIGHUSER | __GFP_ZERO,
						    orders[i], cached);
			if (!pool)
				goto free_pools;
			reserve->pools[cached][i] = pool;
		}
	}
	kobj = kobject_create_and_add("reserve_pool", kernel_kobj);
	if (!kobj)
		goto free_pools;
	/* Publish before sysfs callbacks become visible. */
	reserve_owner = reserve;
	ret = sysfs_create_group(kobj, &reserve_group);
	if (ret) {
		reserve_owner = NULL;
		kobject_put(kobj);
		goto free_pools;
	}
	/* Readers must see completely initialized pools and synchronization. */
	smp_store_release(&heap->reserve, reserve);
	WRITE_ONCE(reserve->ready, true);
	pr_info("neom: registered shrinkable cached/uncached ION reserves\n");
	goto unlock;
free_pools:
	/* No worker or pages exist before successful sysfs publication. */
	for (cached = 0; cached < 2; cached++)
		for (i = 0; i < NUM_ORDERS; i++)
			if (reserve->pools[cached][i])
				ion_page_pool_destroy(reserve->pools[cached][i]);
	kfree(reserve);
	pr_warn("neom: ION reserve setup failed; using original pools\n");
unlock:
	mutex_unlock(&reserve_owner_lock);
}
