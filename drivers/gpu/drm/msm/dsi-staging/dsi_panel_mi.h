/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Xiaomi legacy display parameter ABI used by Sweet vendor displayfeature.
 * Keep this interface separate from the Lineage direct HBM sysfs ABI.
 */
#ifndef _DSI_PANEL_MI_H_
#define _DSI_PANEL_MI_H_

enum xiaomi_disp_param_mode {
	DISPPARAM_DIMMING_OFF = 0x00000E00,
	DISPPARAM_DIMMING_ON  = 0x00000F00,

	DISPPARAM_HBM_ON      = 0x00010000,
	DISPPARAM_HBM_FOD_ON  = 0x00020000,
	DISPPARAM_HBM_FOD2NORM = 0x00030000,
	DISPPARAM_DC_ON       = 0x00040000,
	DISPPARAM_DC_OFF      = 0x00050000,
	DISPPARAM_BC_120HZ    = 0x00060000,
	DISPPARAM_BC_60HZ     = 0x00070000,
	DISPPARAM_HBM_FOD_OFF = 0x000E0000,
	DISPPARAM_HBM_OFF     = 0x000F0000,
};

#endif /* _DSI_PANEL_MI_H_ */
