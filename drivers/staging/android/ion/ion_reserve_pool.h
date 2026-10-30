/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ION_RESERVE_POOL_H
#define _ION_RESERVE_POOL_H

struct device;
struct ion_system_heap;
struct ion_reserve_pool;

#ifdef CONFIG_NEOM_HOS_MEMORY_RESERVES
void ion_reserve_pool_init(struct ion_system_heap *heap, struct device *dev);
struct page *ion_reserve_pool_alloc(struct ion_system_heap *heap,
				    bool cached, unsigned int index);
bool ion_reserve_pool_free(struct ion_system_heap *heap, struct page *page,
			   bool cached, unsigned int index);
int ion_reserve_pool_shrink(struct ion_system_heap *heap, gfp_t gfp_mask,
			    int nr_to_scan);
bool ion_page_pool_free_bounded(struct ion_page_pool *pool, struct page *page,
				unsigned int limit);
#else
static inline void ion_reserve_pool_init(struct ion_system_heap *heap,
					 struct device *dev)
{
}

static inline struct page *ion_reserve_pool_alloc(struct ion_system_heap *heap,
						  bool cached, unsigned int index)
{
	return NULL;
}

static inline bool ion_reserve_pool_free(struct ion_system_heap *heap,
					 struct page *page, bool cached, unsigned int index)
{
	return false;
}

static inline int ion_reserve_pool_shrink(struct ion_system_heap *heap,
					  gfp_t gfp_mask, int nr_to_scan)
{
	return 0;
}
#endif
#endif
