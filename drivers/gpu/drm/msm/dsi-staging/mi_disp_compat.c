// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal Xiaomi mi_disp compatibility ABI for Sweet.
 *
 * Newer HyperOS displayfeature/SF expects /dev/mi_display/disp_feature and
 * MI_DISP ioctls. Sweet predates that userspace ABI and instead exposes the
 * old doze_backlight bridge. Keep Sweet's proven panel command path and only
 * translate the userspace contract here.
 */

#define pr_fmt(fmt) "mi_disp_compat: " fmt

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/ioctl.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "dsi_display.h"
#include "dsi_drm.h"
#include "mi_disp_compat.h"

#define MI_DISPLAY_CLASS		"mi_display"
#define MI_DISP_DEVICE_NAME		"disp_feature"

#define MI_DISP_PRIMARY			0
#define MI_DISP_FEATURE_VERSION		0x0100

#define DISP_FEATURE_DOZE_BRIGHTNESS	3
#define DISP_FEATURE_AOD_TO_NORMAL	16
#define DISP_FEATURE_DOZE_STATE		26
#define DISP_FEATURE_LAYER_HAS_AOD	39
#define DISP_FEATURE_EARLY_SCREEN_STATUS 40
#define DISP_FEATURE_MAX		42

#define MI_DOZE_TO_NORMAL		0
#define MI_DOZE_BRIGHTNESS_HBM		1
#define MI_DOZE_BRIGHTNESS_LBM		2
#define MI_DOZE_BRIGHTNESS_MAX		3

struct mi_disp_base {
	__u32 flag;
	__u32 disp_id;
};

struct mi_disp_version {
	struct mi_disp_base base;
	__u32 version;
};

struct mi_disp_feature_req {
	struct mi_disp_base base;
	__u32 feature_id;
	__s32 feature_val;
	__u32 tx_len;
	__u64 tx_ptr;
	__u32 rx_len;
	__u64 rx_ptr;
};

struct mi_disp_doze_brightness_req {
	struct mi_disp_base base;
	__u32 doze_brightness;
};

#define MI_DISP_IOCTL_VERSION \
	_IOR('D', 0x00, struct mi_disp_version)
#define MI_DISP_IOCTL_SET_FEATURE \
	_IOWR('D', 0x01, struct mi_disp_feature_req)
#define MI_DISP_IOCTL_SET_DOZE_BRIGHTNESS \
	_IOW('D', 0x02, struct mi_disp_doze_brightness_req)
#define MI_DISP_IOCTL_GET_DOZE_BRIGHTNESS \
	_IOR('D', 0x03, struct mi_disp_doze_brightness_req)
#define MI_DISP_IOCTL_GET_FEATURE \
	_IOWR('D', 0x0F, struct mi_disp_feature_req)

static dev_t mi_disp_devt;
static struct cdev mi_disp_cdev;
static struct class *mi_disp_class;
static struct device *mi_disp_device;
static bool mi_disp_registered;

static DEFINE_MUTEX(mi_disp_lock);
static __s32 mi_disp_feature_values[DISP_FEATURE_MAX];
static __u32 mi_disp_cached_doze = MI_DOZE_TO_NORMAL;

static bool mi_disp_valid_display(__u32 disp_id)
{
	/* Sweet has one built-in display; reject secondary-display requests. */
	return disp_id == MI_DISP_PRIMARY;
}

static int mi_disp_doze_to_sweet(__u32 doze)
{
	switch (doze) {
	case MI_DOZE_TO_NORMAL:
		/*
		 * Critical ABI difference:
		 * new mi_disp uses 0 for TO_NORMAL, while Sweet's legacy
		 * doze_backlight contract uses 3 for TO_NORMAL.
		 */
		return DOZE_BRIGHTNESS_TO_NORMAL;
	case MI_DOZE_BRIGHTNESS_HBM:
		return DOZE_BRIGHTNESS_HBM;
	case MI_DOZE_BRIGHTNESS_LBM:
		return DOZE_BRIGHTNESS_LBM;
	default:
		return -EINVAL;
	}
}

static __u32 mi_disp_doze_from_sweet(int doze)
{
	switch (doze) {
	case DOZE_BRIGHTNESS_HBM:
		return MI_DOZE_BRIGHTNESS_HBM;
	case DOZE_BRIGHTNESS_LBM:
		return MI_DOZE_BRIGHTNESS_LBM;
	case DOZE_BRIGHTNESS_TO_NORMAL:
	case DOZE_BRIGHTNESS_INVALID:
	default:
		return MI_DOZE_TO_NORMAL;
	}
}

static int mi_disp_set_doze_brightness(__u32 doze)
{
	struct dsi_display *display;
	int sweet_doze;
	int rc;

	if (doze >= MI_DOZE_BRIGHTNESS_MAX)
		return -EINVAL;

	sweet_doze = mi_disp_doze_to_sweet(doze);
	if (sweet_doze < 0)
		return sweet_doze;

	display = get_main_display();
	if (!display || !display->drm_conn) {
		pr_err("primary display/connector is not ready for doze=%u\n",
		       doze);
		return -ENODEV;
	}

	rc = dsi_bridge_disp_set_doze_backlight(display->drm_conn, sweet_doze);
	if (!rc) {
		mutex_lock(&mi_disp_lock);
		mi_disp_cached_doze = doze;
		mi_disp_feature_values[DISP_FEATURE_DOZE_BRIGHTNESS] = doze;
		mutex_unlock(&mi_disp_lock);
	}

	pr_info("doze %u -> sweet %d, rc=%d\n", doze, sweet_doze, rc);
	return rc;
}

static __u32 mi_disp_get_doze_brightness(void)
{
	struct dsi_display *display;
	__u32 doze;

	display = get_main_display();
	if (display && display->drm_dev)
		return mi_disp_doze_from_sweet(
			READ_ONCE(display->drm_dev->doze_brightness));

	mutex_lock(&mi_disp_lock);
	doze = mi_disp_cached_doze;
	mutex_unlock(&mi_disp_lock);

	return doze;
}

static int mi_disp_set_feature(struct mi_disp_feature_req *req)
{
	int rc = 0;

	if (!mi_disp_valid_display(req->base.disp_id))
		return -EINVAL;
	if (req->feature_id >= DISP_FEATURE_MAX)
		return -EINVAL;

	switch (req->feature_id) {
	case DISP_FEATURE_DOZE_BRIGHTNESS:
		if (req->feature_val < 0)
			return -EINVAL;
		rc = mi_disp_set_doze_brightness((__u32)req->feature_val);
		break;
	case DISP_FEATURE_LAYER_HAS_AOD:
		/*
		 * New MI-SF uses this as a composition/AOD notification.
		 * Sweet has no matching hardware action; accepting and caching it
		 * is sufficient for ABI compatibility.
		 */
		pr_info("layer_has_aod=%d\n", req->feature_val);
		break;
	case DISP_FEATURE_EARLY_SCREEN_STATUS:
		/*
		 * This is a sequencing notification in newer Xiaomi stacks.
		 * Keep it as an acknowledged state update; panel power continues
		 * to be driven by Sweet's existing DRM/DSI flow.
		 */
		pr_info("early_screen_status=%d\n", req->feature_val);
		break;
	case DISP_FEATURE_AOD_TO_NORMAL:
	case DISP_FEATURE_DOZE_STATE:
		pr_debug("feature %u=%d acknowledged\n",
			 req->feature_id, req->feature_val);
		break;
	default:
		/*
		 * Cache known feature IDs rather than returning -EINVAL and
		 * making newer userspace treat the kernel ABI as broken.
		 */
		pr_debug("feature %u=%d cached\n",
			 req->feature_id, req->feature_val);
		break;
	}

	if (!rc && req->feature_id != DISP_FEATURE_DOZE_BRIGHTNESS) {
		mutex_lock(&mi_disp_lock);
		mi_disp_feature_values[req->feature_id] = req->feature_val;
		mutex_unlock(&mi_disp_lock);
	}

	return rc;
}

static int mi_disp_get_feature(struct mi_disp_feature_req *req)
{
	if (!mi_disp_valid_display(req->base.disp_id))
		return -EINVAL;
	if (req->feature_id >= DISP_FEATURE_MAX)
		return -EINVAL;

	if (req->feature_id == DISP_FEATURE_DOZE_BRIGHTNESS) {
		req->feature_val = mi_disp_get_doze_brightness();
		return 0;
	}

	mutex_lock(&mi_disp_lock);
	req->feature_val = mi_disp_feature_values[req->feature_id];
	mutex_unlock(&mi_disp_lock);

	return 0;
}

static long mi_disp_compat_ioctl(struct file *file,
				unsigned int cmd, unsigned long arg)
{
	void __user *argp = (void __user *)arg;
	struct mi_disp_feature_req feature;
	struct mi_disp_doze_brightness_req doze;
	struct mi_disp_version version;
	int rc;

	(void)file;

	switch (cmd) {
	case MI_DISP_IOCTL_VERSION:
		memset(&version, 0, sizeof(version));
		version.version = MI_DISP_FEATURE_VERSION;
		if (copy_to_user(argp, &version, sizeof(version)))
			return -EFAULT;
		return 0;

	case MI_DISP_IOCTL_SET_FEATURE:
		if (copy_from_user(&feature, argp, sizeof(feature)))
			return -EFAULT;
		return mi_disp_set_feature(&feature);

	case MI_DISP_IOCTL_GET_FEATURE:
		if (copy_from_user(&feature, argp, sizeof(feature)))
			return -EFAULT;
		rc = mi_disp_get_feature(&feature);
		if (rc)
			return rc;
		if (copy_to_user(argp, &feature, sizeof(feature)))
			return -EFAULT;
		return 0;

	case MI_DISP_IOCTL_SET_DOZE_BRIGHTNESS:
		if (copy_from_user(&doze, argp, sizeof(doze)))
			return -EFAULT;
		if (!mi_disp_valid_display(doze.base.disp_id))
			return -EINVAL;
		return mi_disp_set_doze_brightness(doze.doze_brightness);

	case MI_DISP_IOCTL_GET_DOZE_BRIGHTNESS:
		if (copy_from_user(&doze, argp, sizeof(doze)))
			return -EFAULT;
		if (!mi_disp_valid_display(doze.base.disp_id))
			return -EINVAL;
		doze.doze_brightness = mi_disp_get_doze_brightness();
		if (copy_to_user(argp, &doze, sizeof(doze)))
			return -EFAULT;
		return 0;

	default:
		pr_debug("unsupported ioctl cmd=0x%x nr=0x%x\n",
			 cmd, _IOC_NR(cmd));
		return -ENOTTY;
	}
}

#ifdef CONFIG_COMPAT
static long mi_disp_compat_compat_ioctl(struct file *file,
				       unsigned int cmd, unsigned long arg)
{
	return mi_disp_compat_ioctl(file, cmd, arg);
}
#endif

static const struct file_operations mi_disp_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = mi_disp_compat_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = mi_disp_compat_compat_ioctl,
#endif
	.llseek = no_llseek,
};

static char *mi_disp_devnode(struct device *dev, umode_t *mode)
{
	(void)mode;
	/* Match Xiaomi's userspace-visible path exactly. */
	return kasprintf(GFP_KERNEL, "%s/%s",
			 MI_DISPLAY_CLASS, dev_name(dev));
}

int mi_disp_compat_init(void)
{
	int rc;

	if (mi_disp_registered)
		return 0;

	rc = alloc_chrdev_region(&mi_disp_devt, 0, 1, MI_DISP_DEVICE_NAME);
	if (rc)
		return rc;

	cdev_init(&mi_disp_cdev, &mi_disp_fops);
	mi_disp_cdev.owner = THIS_MODULE;

	rc = cdev_add(&mi_disp_cdev, mi_disp_devt, 1);
	if (rc)
		goto err_chrdev;

	mi_disp_class = class_create(THIS_MODULE, MI_DISPLAY_CLASS);
	if (IS_ERR(mi_disp_class)) {
		rc = PTR_ERR(mi_disp_class);
		mi_disp_class = NULL;
		goto err_cdev;
	}
	mi_disp_class->devnode = mi_disp_devnode;

	mi_disp_device = device_create(mi_disp_class, NULL, mi_disp_devt,
				       NULL, MI_DISP_DEVICE_NAME);
	if (IS_ERR(mi_disp_device)) {
		rc = PTR_ERR(mi_disp_device);
		mi_disp_device = NULL;
		goto err_class;
	}

	mi_disp_registered = true;
	pr_info("registered /dev/%s/%s ABI v1.0\n",
		MI_DISPLAY_CLASS, MI_DISP_DEVICE_NAME);
	return 0;

err_class:
	class_destroy(mi_disp_class);
	mi_disp_class = NULL;
err_cdev:
	cdev_del(&mi_disp_cdev);
err_chrdev:
	unregister_chrdev_region(mi_disp_devt, 1);
	return rc;
}

void mi_disp_compat_deinit(void)
{
	if (!mi_disp_registered)
		return;

	device_destroy(mi_disp_class, mi_disp_devt);
	class_destroy(mi_disp_class);
	cdev_del(&mi_disp_cdev);
	unregister_chrdev_region(mi_disp_devt, 1);

	mi_disp_device = NULL;
	mi_disp_class = NULL;
	mi_disp_registered = false;
}
