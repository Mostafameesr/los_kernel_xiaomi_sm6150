/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MSM_VIDC_QC2_COMPAT_H_
#define _MSM_VIDC_QC2_COMPAT_H_

/*
 * Preparation primitives only. No ioctl handler selects this namespace yet.
 * Never infer the namespace from process bitness or a colliding control ID.
 */
#include <linux/types.h>

enum msm_vidc_qc2_namespace {
	MSM_VIDC_QC2_LEGACY = 0,
	MSM_VIDC_QC2_SPRING = 1,
};

#define QC2_SPRING_SECURE		0x00992001U
#define QC2_SPRING_LOW_LATENCY		0x00992003U
#define QC2_SPRING_OUTPUT_ORDER		0x00990b8eU
#define QC2_SPRING_THUMBNAIL		0x00992029U
#define QC2_SPRING_PRIORITY		0x0099202aU
#define QC2_SPRING_CLIENT_ID		0x00992041U

struct msm_vidc_qc2_control {
	__u32 id;
	__s32 value;
};

/* Failure leaves *out untouched. Success does not submit anything to firmware. */
int msm_vidc_qc2_translate_ctrl(enum msm_vidc_qc2_namespace ns,
		const struct msm_vidc_qc2_control *in,
		struct msm_vidc_qc2_control *out);

struct msm_vidc_qc2_dmabuf_plane {
	__s32 fd;
	__u32 length;
	__u32 bytesused;
	__u32 data_offset;
};

/*
 * Shape of the information needed by msm_vidc_qbuf's legacy reserved[] ABI.
 * This is not a native v4l2_plane, an imported DMA mapping or an owning fd.
 */
struct msm_vidc_qc2_legacy_plane {
	__u32 length;
	__u32 bytesused;
	__u32 data_offset;
	__s32 fd;
	__u32 reserved[11];
};

/*
 * Copies descriptor fields only. Caller must separately pin the DMA-BUF,
 * validate the negotiated plane format, and manage fences and completion.
 * No success from this helper authorizes a QBUF or advertises VB2_DMABUF.
 */
int msm_vidc_qc2_plane_to_legacy(
		const struct msm_vidc_qc2_dmabuf_plane *in,
		struct msm_vidc_qc2_legacy_plane *out);

#endif
