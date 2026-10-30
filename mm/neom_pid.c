// SPDX-License-Identifier: GPL-2.0
/* PID registrations retain identity across exit and numeric PID reuse. */
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/neom_reserve.h>
#include <linux/pid_namespace.h>
#include <linux/sched/signal.h>

/* Caller holds list->lock and rcu_read_lock(). */
static void neom_pid_list_prune(struct neom_pid_list *list)
{
	int i;

	for (i = 0; i < NEOM_RESERVE_MAX_PIDS; i++) {
		struct pid *pid = list->pids[i];

		if (pid && !pid_task(pid, PIDTYPE_TGID)) {
			list->pids[i] = NULL;
			put_pid(pid);
		}
	}
}

bool neom_pid_list_contains(struct neom_pid_list *list)
{
	unsigned long flags;
	bool found = false;
	int i;

	spin_lock_irqsave(&list->lock, flags);
	for (i = 0; i < NEOM_RESERVE_MAX_PIDS; i++)
		if (list->pids[i] == task_tgid(current)) {
			found = true;
			break;
		}
	spin_unlock_irqrestore(&list->lock, flags);
	return found;
}

bool neom_pid_list_has_live(struct neom_pid_list *list)
{
	unsigned long flags;
	bool found = false;
	int i;

	rcu_read_lock();
	spin_lock_irqsave(&list->lock, flags);
	neom_pid_list_prune(list);
	for (i = 0; i < NEOM_RESERVE_MAX_PIDS; i++)
		if (list->pids[i]) {
			found = true;
			break;
		}
	spin_unlock_irqrestore(&list->lock, flags);
	rcu_read_unlock();
	return found;
}

ssize_t neom_pid_list_show(struct neom_pid_list *list, char *buf)
{
	unsigned long flags;
	ssize_t len = 0;
	int i;

	rcu_read_lock();
	spin_lock_irqsave(&list->lock, flags);
	neom_pid_list_prune(list);
	for (i = 0; i < NEOM_RESERVE_MAX_PIDS; i++)
		if (list->pids[i])
			len += scnprintf(buf + len, PAGE_SIZE - len, "%d ",
					 pid_nr(list->pids[i]));
	len += scnprintf(buf + len, PAGE_SIZE - len, "\n");
	spin_unlock_irqrestore(&list->lock, flags);
	rcu_read_unlock();
	return len;
}

ssize_t neom_pid_list_store(struct neom_pid_list *list, const char *buf,
			    size_t count)
{
	struct pid *registration = NULL, *input;
	struct task_struct *task;
	unsigned long flags;
	int value, i, empty = -1, ret;

	if (task_active_pid_ns(current) != &init_pid_ns)
		return -EINVAL;
	ret = kstrtoint(buf, 0, &value);
	if (ret)
		return ret;
	if (!value || value == INT_MIN)
		return -EINVAL;

	if (value > 0) {
		input = find_get_pid(value);
		if (!input)
			return -ESRCH;
		rcu_read_lock();
		task = pid_task(input, PIDTYPE_PID);
		if (task && task->mm)
			registration = get_pid(task_tgid(task));
		rcu_read_unlock();
		put_pid(input);
		if (!registration)
			return -ESRCH;
	}

	ret = -ENOSPC;
	rcu_read_lock();
	spin_lock_irqsave(&list->lock, flags);
	neom_pid_list_prune(list);
	for (i = 0; i < NEOM_RESERVE_MAX_PIDS; i++) {
		struct pid *pid = list->pids[i];

		if (!pid) {
			if (empty < 0)
				empty = i;
			continue;
		}
		if (value > 0 && pid == registration) {
			ret = 0;
			goto unlock;
		}
		if (value < 0 && pid_nr(pid) == -value) {
			list->pids[i] = NULL;
			put_pid(pid);
		}
	}
	if (value < 0) {
		ret = 0;
	} else if (empty >= 0) {
		list->pids[empty] = registration;
		registration = NULL;
		ret = 0;
	}
unlock:
	spin_unlock_irqrestore(&list->lock, flags);
	rcu_read_unlock();
	put_pid(registration);
	return ret ? ret : count;
}
