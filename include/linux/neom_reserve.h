/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_NEOM_RESERVE_H
#define _LINUX_NEOM_RESERVE_H

#include <linux/gfp.h>
#include <linux/nodemask.h>
#include <linux/pid.h>
#include <linux/spinlock.h>

#define NEOM_RESERVE_MAX_PIDS 16

struct neom_pid_list {
	/* Protects the retained PID references and registration slots. */
	spinlock_t lock;
	struct pid *pids[NEOM_RESERVE_MAX_PIDS];
};

#define NEOM_PID_LIST_INIT(name) \
	{ .lock = __SPIN_LOCK_UNLOCKED(name.lock) }

#ifdef CONFIG_NEOM_HOS_MEMORY_RESERVES
bool neom_pid_list_contains(struct neom_pid_list *list);
bool neom_pid_list_has_live(struct neom_pid_list *list);
ssize_t neom_pid_list_show(struct neom_pid_list *list, char *buf);
ssize_t neom_pid_list_store(struct neom_pid_list *list, const char *buf,
			    size_t count);
struct page *neom_mempool_alloc(gfp_t gfp_mask, unsigned int order,
				const nodemask_t *nodemask);
#else
static inline struct page *neom_mempool_alloc(gfp_t gfp_mask,
					      unsigned int order, const nodemask_t *nodemask)
{
	return NULL;
}
#endif
#endif
