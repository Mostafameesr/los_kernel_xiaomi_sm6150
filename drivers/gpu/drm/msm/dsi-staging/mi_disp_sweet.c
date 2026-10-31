// SPDX-License-Identifier: GPL-2.0
/* Xiaomi display ABI backed by Sweet panel commands and state notifications. */
#include <linux/compat.h>
#include <linux/fs.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/rwsem.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include "dsi_display.h"
#include "dsi_panel.h"
#include "mi_disp_sweet.h"

#define MI_EVENT_SLOTS 256
#define MI_MAX_PENDING 64

struct mi_event {
	struct disp_event base;
	u32 value;
};

struct mi_client {
	struct mi_disp_sweet *ctx;
	struct list_head link;
	wait_queue_head_t wait;
	struct mutex read_lock;
	unsigned long mask;
	struct mi_event events[MI_EVENT_SLOTS];
	u16 head, tail, count;
};

struct mi_disp_sweet {
	struct miscdevice misc;
	struct kref ref;
	struct rw_semaphore gate;
	struct dsi_display *display;
	struct workqueue_struct *worker;
	wait_queue_head_t state_wait;
	spinlock_t clients_lock;
	struct list_head clients;
	bool stopping, ready;
	atomic_t pending;
	atomic_t completed;
	atomic_t last_error;
	atomic_t dropped;
	struct mutex count_lock;
	s32 count_info[6];
	char build_version[100];
};

struct mi_work {
	struct work_struct work;
	struct mi_disp_sweet *ctx;
	u32 feature;
	int value;
};

static DEFINE_MUTEX(mi_registry_lock);
static struct mi_disp_sweet *mi_primary;

static bool mi_feature_supported(u32 id)
{
	return id == DISP_FEATURE_DIMMING || id == DISP_FEATURE_HBM ||
		id == DISP_FEATURE_DOZE_BRIGHTNESS ||
		id == DISP_FEATURE_FULLSCREEN_AOD_STATUS ||
		id == DISP_FEATURE_LAYER_HAS_AOD ||
		id == DISP_FEATURE_EARLY_SCREEN_STATUS;
}

static bool mi_event_supported(u32 type)
{
	return type == MI_DISP_EVENT_POWER || type == MI_DISP_EVENT_BACKLIGHT ||
		type == MI_DISP_EVENT_DOZE || type == MI_DISP_EVENT_FPS ||
		type == MI_DISP_EVENT_51_BRIGHTNESS || type == MI_DISP_EVENT_HBM ||
		type == MI_DISP_EVENT_FULLSCREEN_AOD;
}

static int mi_check_feature(u32 id, int value)
{
	if (!mi_feature_supported(id))
		return -EOPNOTSUPP;
	if (id == DISP_FEATURE_DOZE_BRIGHTNESS)
		return value >= 0 && value <= 2 ? 0 : -EINVAL;
	return value == 0 || value == 1 ? 0 : -EINVAL;
}

/* Callers hold panel_lock; unregister clears the pointer under that lock. */
void mi_disp_sweet_notify(struct dsi_panel *panel, u32 type, u32 value)
{
	struct mi_disp_sweet *ctx = panel->mi_disp;
	struct mi_client *client;
	struct mi_event *event;
	unsigned long flags;

	if (!ctx || READ_ONCE(ctx->stopping) || !mi_event_supported(type))
		return;
	spin_lock_irqsave(&ctx->clients_lock, flags);
	list_for_each_entry(client, &ctx->clients, link) {
		if (!(client->mask & BIT(type)))
			continue;
		if (client->count == MI_EVENT_SLOTS) {
			atomic_inc(&ctx->dropped);
			continue;
		}
		event = &client->events[client->tail];
		event->base.disp_id = 0;
		event->base.type = type;
		event->base.length = sizeof(*event);
		event->value = value;
		client->tail = (client->tail + 1) % MI_EVENT_SLOTS;
		client->count++;
		wake_up_interruptible(&client->wait);
	}
	spin_unlock_irqrestore(&ctx->clients_lock, flags);
}

void mi_disp_sweet_state_changed(struct dsi_panel *panel)
{
	struct mi_disp_sweet *ctx = panel->mi_disp;

	if (ctx) {
		WRITE_ONCE(ctx->ready, panel->panel_initialized);
		wake_up_all(&ctx->state_wait);
	}
}

static void mi_free(struct kref *ref)
{
	struct mi_disp_sweet *ctx = container_of(ref, struct mi_disp_sweet, ref);

	kfree(ctx);
}

static int mi_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct mi_disp_sweet *ctx = container_of(misc, struct mi_disp_sweet, misc);
	struct mi_client *client;
	unsigned long flags;

	if (READ_ONCE(ctx->stopping))
		return -ENODEV;
	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return -ENOMEM;
	kref_get(&ctx->ref);
	client->ctx = ctx;
	init_waitqueue_head(&client->wait);
	mutex_init(&client->read_lock);
	spin_lock_irqsave(&ctx->clients_lock, flags);
	list_add_tail(&client->link, &ctx->clients);
	spin_unlock_irqrestore(&ctx->clients_lock, flags);
	file->private_data = client;
	return nonseekable_open(inode, file);
}

static int mi_release(struct inode *inode, struct file *file)
{
	struct mi_client *client = file->private_data;
	struct mi_disp_sweet *ctx = client->ctx;
	unsigned long flags;

	spin_lock_irqsave(&ctx->clients_lock, flags);
	list_del(&client->link);
	spin_unlock_irqrestore(&ctx->clients_lock, flags);
	kfree(client);
	kref_put(&ctx->ref, mi_free);
	return 0;
}

static ssize_t mi_read(struct file *file, char __user *buf, size_t size,
		       loff_t *offset)
{
	struct mi_client *client = file->private_data;
	struct mi_disp_sweet *ctx = client->ctx;
	struct mi_event event;
	unsigned long flags;
	ssize_t done = 0;
	int rc;

	if (!size)
		return 0;
	if (size < sizeof(event))
		return -EINVAL;
	rc = mutex_lock_interruptible(&client->read_lock);
	if (rc)
		return rc;
	while (size - done >= sizeof(event)) {
		spin_lock_irqsave(&ctx->clients_lock, flags);
		if (!client->count) {
			spin_unlock_irqrestore(&ctx->clients_lock, flags);
			if (done || READ_ONCE(ctx->stopping))
				break;
			if (file->f_flags & O_NONBLOCK) {
				done = -EAGAIN;
				break;
			}
			rc = wait_event_interruptible(client->wait,
				READ_ONCE(client->count) || READ_ONCE(ctx->stopping));
			if (rc) {
				done = rc;
				break;
			}
			continue;
		}
		event = client->events[client->head];
		spin_unlock_irqrestore(&ctx->clients_lock, flags);
		if (copy_to_user(buf + done, &event, sizeof(event))) {
			if (!done)
				done = -EFAULT;
			break;
		}
		spin_lock_irqsave(&ctx->clients_lock, flags);
		client->head = (client->head + 1) % MI_EVENT_SLOTS;
		client->count--;
		spin_unlock_irqrestore(&ctx->clients_lock, flags);
		done += sizeof(event);
	}
	mutex_unlock(&client->read_lock);
	return done;
}

static unsigned int mi_poll(struct file *file, poll_table *wait)
{
	struct mi_client *client = file->private_data;
	unsigned int mask = 0;

	poll_wait(file, &client->wait, wait);
	if (READ_ONCE(client->count))
		mask |= POLLIN | POLLRDNORM;
	if (READ_ONCE(client->ctx->stopping))
		mask |= POLLHUP;
	return mask;
}

static void mi_apply_work(struct work_struct *work)
{
	struct mi_work *item = container_of(work, struct mi_work, work);
	struct mi_disp_sweet *ctx = item->ctx;
	int rc = 0;

	/* No display/panel/lifetime lock is held while waiting for initialization. */
	if (item->feature == DISP_FEATURE_DIMMING ||
	    item->feature == DISP_FEATURE_DOZE_BRIGHTNESS)
		wait_event(ctx->state_wait, READ_ONCE(ctx->ready) ||
			   READ_ONCE(ctx->stopping));
	down_read(&ctx->gate);
	if (READ_ONCE(ctx->stopping) || !ctx->display)
		rc = -ENODEV;
	else
		rc = dsi_display_set_mi_feature(ctx->display, item->feature, item->value);
	up_read(&ctx->gate);
	atomic_set(&ctx->last_error, rc);
	atomic_inc(&ctx->completed);
	atomic_dec(&ctx->pending);
	if (rc)
		pr_err_ratelimited("mi_disp: async feature %u failed: %d\n", item->feature, rc);
	kfree(item);
}

static int mi_set_feature(struct mi_disp_sweet *ctx, u32 feature, int value, u32 flag)
{
	struct mi_work *item;
	int rc;

	rc = mi_check_feature(feature, value);
	if (rc)
		return rc;
	/* Metadata does not need panel initialization or a DSI worker. */
	if (flag == MI_DISP_FLAG_BLOCK ||
	    feature == DISP_FEATURE_FULLSCREEN_AOD_STATUS ||
	    feature == DISP_FEATURE_LAYER_HAS_AOD)
		return dsi_display_set_mi_feature(ctx->display, feature, value);
	if (atomic_inc_return(&ctx->pending) > MI_MAX_PENDING) {
		atomic_dec(&ctx->pending);
		return -EBUSY;
	}
	item = kzalloc(sizeof(*item), GFP_KERNEL);
	if (!item) {
		atomic_dec(&ctx->pending);
		return -ENOMEM;
	}
	item->ctx = ctx;
	item->feature = feature;
	item->value = value;
	INIT_WORK(&item->work, mi_apply_work);
	queue_work(ctx->worker, &item->work);
	return 0;
}

static int mi_get_feature(struct dsi_panel *panel, u32 feature, int *value)
{
	int rc = 0;

	mutex_lock(&panel->panel_lock);
	switch (feature) {
	case DISP_FEATURE_DIMMING:
		if (panel->mi_dimming_state < 0)
			rc = -EAGAIN;
		else
			*value = panel->mi_dimming_state;
		break;
	case DISP_FEATURE_HBM:
		*value = panel->hbm_applied;
		break;
	case DISP_FEATURE_DOZE_BRIGHTNESS:
		*value = panel->doze_enabled ?
			(panel->doze_mode == DSI_DOZE_HBM ? 1 : 2) : 0;
		break;
	case DISP_FEATURE_FULLSCREEN_AOD_STATUS:
		*value = panel->mi_fullscreen_aod;
		break;
	case DISP_FEATURE_LAYER_HAS_AOD:
		*value = panel->mi_layer_has_aod;
		break;
	case DISP_FEATURE_EARLY_SCREEN_STATUS:
		*value = panel->mi_early_screen;
		break;
	default:
		rc = -EOPNOTSUPP;
	}
	mutex_unlock(&panel->panel_lock);
	return rc;
}

static int mi_count_info(struct mi_disp_sweet *ctx, struct disp_count_info_req *req)
{
	char *version = NULL;

	if (req->count_info_type >= ARRAY_SIZE(ctx->count_info) ||
	    req->rx_len || req->rx_ptr)
		return -EINVAL;
	if (req->count_info_type == 1) {
		if (!req->tx_len || req->tx_len >= sizeof(ctx->build_version))
			return -EINVAL;
		version = memdup_user_nul(u64_to_user_ptr(req->tx_ptr), req->tx_len);
		if (IS_ERR(version))
			return PTR_ERR(version);
	} else if (req->tx_len || req->tx_ptr) {
		return -EINVAL;
	}
	mutex_lock(&ctx->count_lock);
	ctx->count_info[req->count_info_type] = req->count_info_val;
	if (version)
		strscpy(ctx->build_version, version, sizeof(ctx->build_version));
	mutex_unlock(&ctx->count_lock);
	kfree(version);
	return 0;
}

static long mi_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct mi_client *client = file->private_data;
	struct mi_disp_sweet *ctx = client->ctx;
	union {
		struct disp_base base;
		struct disp_version version;
		struct disp_feature_req feature;
		struct disp_doze_brightness_req doze;
		struct disp_event_req event;
		struct disp_fps_info fps;
		struct disp_count_info_req count;
	} req;
	void __user *user = (void __user *)arg;
	unsigned long flags;
	size_t size;
	int rc;

	switch (cmd) {
	case MI_DISP_IOCTL_VERSION:
		size = sizeof(req.version);
		break;
	case MI_DISP_IOCTL_SET_FEATURE:
	case MI_DISP_IOCTL_GET_FEATURE:
		size = sizeof(req.feature);
		break;
	case MI_DISP_IOCTL_SET_DOZE_BRIGHTNESS:
	case MI_DISP_IOCTL_GET_DOZE_BRIGHTNESS:
		size = sizeof(req.doze);
		break;
	case MI_DISP_IOCTL_REGISTER_EVENT:
	case MI_DISP_IOCTL_DEREGISTER_EVENT:
		size = sizeof(req.event);
		break;
	case MI_DISP_IOCTL_GET_FPS:
		size = sizeof(req.fps);
		break;
	case MI_DISP_IOCTL_SET_COUNT_INFO:
		size = sizeof(req.count);
		break;
	default:
		return -ENOTTY;
	}
	memset(&req, 0, sizeof(req));
	if (copy_from_user(&req, user, size))
		return -EFAULT;
	if (req.base.disp_id || req.base.flag > MI_DISP_FLAG_NONBLOCK)
		return -EINVAL;
	if (cmd == MI_DISP_IOCTL_SET_FEATURE || cmd == MI_DISP_IOCTL_GET_FEATURE) {
		if (req.feature.tx_len || req.feature.rx_len ||
		    req.feature.tx_ptr || req.feature.rx_ptr)
			return -EOPNOTSUPP;
	}
	down_read(&ctx->gate);
	if (READ_ONCE(ctx->stopping) || !ctx->display) {
		rc = -ENODEV;
		goto unlock;
	}
	switch (cmd) {
	case MI_DISP_IOCTL_VERSION:
		req.version.version = MI_DISP_FEATURE_VERSION;
		rc = 0;
		break;
	case MI_DISP_IOCTL_SET_FEATURE:
		rc = mi_set_feature(ctx, req.feature.feature_id,
				    req.feature.feature_val, req.base.flag);
		break;
	case MI_DISP_IOCTL_GET_FEATURE:
		rc = mi_get_feature(ctx->display->panel, req.feature.feature_id,
				    &req.feature.feature_val);
		break;
	case MI_DISP_IOCTL_SET_DOZE_BRIGHTNESS:
		if (req.doze.doze_brightness > 2) {
			rc = -EINVAL;
			break;
		}
		rc = mi_set_feature(ctx, DISP_FEATURE_DOZE_BRIGHTNESS,
				    req.doze.doze_brightness, req.base.flag);
		break;
	case MI_DISP_IOCTL_GET_DOZE_BRIGHTNESS:
		rc = mi_get_feature(ctx->display->panel, DISP_FEATURE_DOZE_BRIGHTNESS,
				    (int *)&req.doze.doze_brightness);
		break;
	case MI_DISP_IOCTL_REGISTER_EVENT:
	case MI_DISP_IOCTL_DEREGISTER_EVENT:
		if (!mi_event_supported(req.event.type)) {
			rc = -EOPNOTSUPP;
			break;
		}
		spin_lock_irqsave(&ctx->clients_lock, flags);
		if (cmd == MI_DISP_IOCTL_REGISTER_EVENT)
			client->mask |= BIT(req.event.type);
		else
			client->mask &= ~BIT(req.event.type);
		spin_unlock_irqrestore(&ctx->clients_lock, flags);
		rc = 0;
		break;
	case MI_DISP_IOCTL_GET_FPS:
		mutex_lock(&ctx->display->panel->panel_lock);
		if (!ctx->display->panel->cur_mode) {
			rc = -EAGAIN;
		} else {
			req.fps.fps = ctx->display->panel->cur_mode->timing.refresh_rate;
			rc = 0;
		}
		mutex_unlock(&ctx->display->panel->panel_lock);
		break;
	case MI_DISP_IOCTL_SET_COUNT_INFO:
		rc = mi_count_info(ctx, &req.count);
		break;
	default:
		rc = -ENOTTY;
	}
	if (!rc && (_IOC_DIR(cmd) & _IOC_READ) && copy_to_user(user, &req, size))
		rc = -EFAULT;
unlock:
	up_read(&ctx->gate);
	return rc;
}

#ifdef CONFIG_COMPAT
static long mi_compat_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	return mi_ioctl(file, cmd, (unsigned long)compat_ptr(arg));
}
#endif

static const struct file_operations mi_fops = {
	.owner = THIS_MODULE,
	.open = mi_open,
	.release = mi_release,
	.read = mi_read,
	.poll = mi_poll,
	.unlocked_ioctl = mi_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = mi_compat_ioctl,
#endif
	.llseek = no_llseek,
};

static ssize_t status_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct miscdevice *misc = dev_get_drvdata(dev);
	struct mi_disp_sweet *ctx = container_of(misc, struct mi_disp_sweet, misc);
	ssize_t len;

	mutex_lock(&ctx->count_lock);
	len = scnprintf(buf, PAGE_SIZE,
		"pending=%d completed=%d last_async_error=%d dropped_events=%d\n"
		"sf_power=%d frame_drops=%d timer=%d tpidle=%d true_tone=%d build=%s\n",
		atomic_read(&ctx->pending), atomic_read(&ctx->completed),
		atomic_read(&ctx->last_error), atomic_read(&ctx->dropped),
		ctx->count_info[0], ctx->count_info[2], ctx->count_info[3],
		ctx->count_info[4], ctx->count_info[5], ctx->build_version);
	mutex_unlock(&ctx->count_lock);
	return len;
}

static DEVICE_ATTR_RO(status);

static struct attribute *mi_attrs[] = {
	&dev_attr_status.attr,
	NULL,
};

static const struct attribute_group mi_group = {
	.attrs = mi_attrs,
};

static const struct attribute_group *mi_groups[] = {
	&mi_group,
	NULL,
};

int mi_disp_sweet_register(struct dsi_display *display)
{
	struct mi_disp_sweet *ctx;
	int rc = 0;

	if (!display || !display->panel || !display->display_type)
		return -EINVAL;
	/* Connector registration precedes mode enumeration on cold boot. */
	if (strcmp(display->display_type, "primary"))
		return 0;
	mutex_lock(&mi_registry_lock);
	if (mi_primary)
		goto unlock;
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		rc = -ENOMEM;
		goto unlock;
	}
	kref_init(&ctx->ref);
	init_rwsem(&ctx->gate);
	init_waitqueue_head(&ctx->state_wait);
	spin_lock_init(&ctx->clients_lock);
	INIT_LIST_HEAD(&ctx->clients);
	mutex_init(&ctx->count_lock);
	ctx->display = display;
	ctx->worker = alloc_ordered_workqueue("mi_disp_sweet", WQ_MEM_RECLAIM);
	if (!ctx->worker) {
		rc = -ENOMEM;
		goto free_ctx;
	}
	ctx->misc.minor = MISC_DYNAMIC_MINOR;
	ctx->misc.name = "disp_feature";
	ctx->misc.nodename = "mi_display/disp_feature";
	ctx->misc.mode = 0660;
	ctx->misc.parent = &display->pdev->dev;
	ctx->misc.fops = &mi_fops;
	ctx->misc.groups = mi_groups;
	mutex_lock(&display->panel->panel_lock);
	ctx->ready = display->panel->panel_initialized;
	display->panel->mi_dimming_state = -1;
	display->panel->mi_disp = ctx;
	mutex_unlock(&display->panel->panel_lock);
	rc = misc_register(&ctx->misc);
	if (rc) {
		mutex_lock(&display->panel->panel_lock);
		display->panel->mi_disp = NULL;
		mutex_unlock(&display->panel->panel_lock);
		goto free_worker;
	}
	mi_primary = ctx;
	goto unlock;
free_worker:
	destroy_workqueue(ctx->worker);
free_ctx:
	kref_put(&ctx->ref, mi_free);
unlock:
	mutex_unlock(&mi_registry_lock);
	return rc;
}

void mi_disp_sweet_unregister(struct dsi_display *display)
{
	struct mi_disp_sweet *ctx;
	struct mi_client *client;
	unsigned long flags;

	mutex_lock(&mi_registry_lock);
	ctx = mi_primary;
	if (!ctx || ctx->display != display)
		goto unlock;
	WRITE_ONCE(ctx->stopping, true);
	misc_deregister(&ctx->misc);
	wake_up_all(&ctx->state_wait);
	/* Drain ioctl enqueuers before flushing, without blocking worker exit. */
	down_write(&ctx->gate);
	mutex_lock(&display->panel->panel_lock);
	display->panel->mi_disp = NULL;
	mutex_unlock(&display->panel->panel_lock);
	ctx->display = NULL;
	up_write(&ctx->gate);
	flush_workqueue(ctx->worker);
	spin_lock_irqsave(&ctx->clients_lock, flags);
	list_for_each_entry(client, &ctx->clients, link)
		wake_up_interruptible(&client->wait);
	spin_unlock_irqrestore(&ctx->clients_lock, flags);
	destroy_workqueue(ctx->worker);
	mi_primary = NULL;
	kref_put(&ctx->ref, mi_free);
unlock:
	mutex_unlock(&mi_registry_lock);
}

