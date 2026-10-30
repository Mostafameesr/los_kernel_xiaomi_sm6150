/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_MI_DISP_H_
#define _UAPI_MI_DISP_H_

#include <linux/types.h>
#include <asm/ioctl.h>

#define MI_DISP_FLAG_BLOCK 0
#define MI_DISP_FLAG_NONBLOCK 1
#define MI_DISP_FEATURE_VERSION 0x100

struct disp_base {
	__u32 flag;
	__u32 disp_id;
};

struct disp_version {
	struct disp_base base;
	__u32 version;
};

struct disp_feature_req {
	struct disp_base base;
	__u32 feature_id;
	__s32 feature_val;
	__u32 tx_len;
	__aligned_u64 tx_ptr;
	__u32 rx_len;
	__aligned_u64 rx_ptr;
};

struct disp_doze_brightness_req {
	struct disp_base base;
	__u32 doze_brightness;
};

struct disp_fps_info {
	struct disp_base base;
	__u32 fps;
};

struct disp_event_req {
	struct disp_base base;
	__u32 type;
};

struct disp_event {
	__s32 disp_id;
	__u32 type;
	__u32 length;
};

struct disp_count_info_req {
	struct disp_base base;
	__u32 count_info_type;
	__s32 count_info_val;
	__u32 tx_len;
	__aligned_u64 tx_ptr;
	__u32 rx_len;
	__aligned_u64 rx_ptr;
};

enum disp_feature_id {
	DISP_FEATURE_DIMMING = 0,
	DISP_FEATURE_HBM = 1,
	DISP_FEATURE_DOZE_BRIGHTNESS = 3,
	DISP_FEATURE_FULLSCREEN_AOD_STATUS = 35,
	DISP_FEATURE_LAYER_HAS_AOD = 39,
	DISP_FEATURE_EARLY_SCREEN_STATUS = 40,
};

enum disp_event_type {
	MI_DISP_EVENT_POWER = 0,
	MI_DISP_EVENT_BACKLIGHT = 1,
	MI_DISP_EVENT_DOZE = 3,
	MI_DISP_EVENT_FPS = 4,
	MI_DISP_EVENT_51_BRIGHTNESS = 6,
	MI_DISP_EVENT_HBM = 7,
	MI_DISP_EVENT_FULLSCREEN_AOD = 15,
};

#define MI_DISP_IOCTL_VERSION _IOR('D', 0x00, struct disp_version)
#define MI_DISP_IOCTL_SET_FEATURE _IOWR('D', 0x01, struct disp_feature_req)
#define MI_DISP_IOCTL_SET_DOZE_BRIGHTNESS _IOW('D', 0x02, struct disp_doze_brightness_req)
#define MI_DISP_IOCTL_GET_DOZE_BRIGHTNESS _IOR('D', 0x03, struct disp_doze_brightness_req)
#define MI_DISP_IOCTL_GET_FPS _IOR('D', 0x06, struct disp_fps_info)
#define MI_DISP_IOCTL_REGISTER_EVENT _IOW('D', 0x07, struct disp_event_req)
#define MI_DISP_IOCTL_DEREGISTER_EVENT _IOW('D', 0x08, struct disp_event_req)
#define MI_DISP_IOCTL_SET_COUNT_INFO _IOWR('D', 0x0d, struct disp_count_info_req)
#define MI_DISP_IOCTL_GET_FEATURE _IOWR('D', 0x0f, struct disp_feature_req)

#endif
