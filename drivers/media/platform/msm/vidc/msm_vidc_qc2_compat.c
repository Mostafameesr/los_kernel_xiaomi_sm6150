// SPDX-License-Identifier: GPL-2.0-only
/*
 * Inert preparation for a separately selected Codec2 ABI. Deliberately not
 * linked into msm-vidc: metadata queues and the ownership bridge are unfinished.
 */
#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif
#include <linux/v4l2-controls.h>
#include "msm_vidc_qc2_compat.h"

int msm_vidc_qc2_translate_ctrl(enum msm_vidc_qc2_namespace ns,
		const struct msm_vidc_qc2_control *in,
		struct msm_vidc_qc2_control *out)
{
	struct msm_vidc_qc2_control translated;

	if (!in || !out)
		return -EINVAL;

	if (ns == MSM_VIDC_QC2_LEGACY) {
		*out = *in;
		return 0;
	}
	if (ns != MSM_VIDC_QC2_SPRING)
		return -EINVAL;

	translated = *in;
	switch (in->id) {
	case QC2_SPRING_OUTPUT_ORDER:
		/* Donor's own Str(OutputOrder): 0 display, 1 decode. */
		translated.id = V4L2_CID_MPEG_VIDC_VIDEO_OUTPUT_ORDER;
		break;
	case QC2_SPRING_LOW_LATENCY:
		/* Donor's own Str(LowLatencyMode): 0 disabled, 1 enabled. */
		translated.id = V4L2_CID_MPEG_VIDC_VIDEO_LOWLATENCY_MODE;
		break;
	case QC2_SPRING_THUMBNAIL:
		translated.id = V4L2_CID_MPEG_VIDC_VIDEO_SYNC_FRAME_DECODE;
		break;
	case QC2_SPRING_SECURE:
		/* Legacy SECURE is a one-way BUTTON, not a boolean setter. */
	case QC2_SPRING_PRIORITY:
		/* Legacy implementation and public priority enum disagree. */
	case QC2_SPRING_CLIENT_ID:
		/* No verified legacy equivalent. Never pass a colliding ID. */
	default:
		return -EOPNOTSUPP;
	}

	if (in->value != 0 && in->value != 1)
		return -EINVAL;

	*out = translated;
	return 0;
}

int msm_vidc_qc2_plane_to_legacy(
		const struct msm_vidc_qc2_dmabuf_plane *in,
		struct msm_vidc_qc2_legacy_plane *out)
{
	struct msm_vidc_qc2_legacy_plane translated = { 0 };

	if (!in || !out)
		return -EINVAL;
	if (in->fd < 0 || !in->length ||
		in->bytesused > in->length ||
		in->data_offset > in->length ||
		(in->bytesused && in->data_offset > in->bytesused))
		return -EINVAL;

	translated.length = in->length;
	translated.bytesused = in->bytesused;
	translated.data_offset = in->data_offset;
	translated.fd = in->fd;
	translated.reserved[0] = (__u32)in->fd;
	translated.reserved[1] = in->data_offset;
	*out = translated;
	return 0;
}
