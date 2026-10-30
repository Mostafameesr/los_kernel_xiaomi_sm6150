/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _MI_DISP_SWEET_H_
#define _MI_DISP_SWEET_H_

#include <linux/types.h>
#include <uapi/drm/mi_disp.h>

struct dsi_panel;
struct dsi_display;
struct mi_disp_sweet;

#ifdef CONFIG_MACH_XIAOMI_SWEET
int mi_disp_sweet_register(struct dsi_display *display);
void mi_disp_sweet_unregister(struct dsi_display *display);
void mi_disp_sweet_notify(struct dsi_panel *panel, u32 type, u32 value);
void mi_disp_sweet_state_changed(struct dsi_panel *panel);
#else
static inline int mi_disp_sweet_register(struct dsi_display *display)
{
	return 0;
}
static inline void mi_disp_sweet_unregister(struct dsi_display *display)
{
}
static inline void mi_disp_sweet_notify(struct dsi_panel *panel, u32 type, u32 value)
{
}
static inline void mi_disp_sweet_state_changed(struct dsi_panel *panel)
{
}
#endif

#endif
