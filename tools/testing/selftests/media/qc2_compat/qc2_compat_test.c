// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <linux/v4l2-controls.h>
#include "msm_vidc_qc2_compat.h"

static unsigned int checks;
#define CHECK(expr) do { assert(expr); checks++; } while (0)

static void test_controls(void)
{
	static const struct {
		__u32 modern;
		__u32 legacy;
	} mappings[] = {
		{ 0x00990b8eU, 0x00992003U },
		{ 0x00992003U, 0x00992038U },
		{ 0x00992029U, 0x0099200fU },
	};
	static const __u32 unsupported[] = {
		0x00992001U, 0x0099202aU, 0x00992041U,
		0x00992010U, 0x009920ffU, 0x00980900U,
	};
	static const __s32 bad_values[] = { -1, 2, INT_MIN, INT_MAX };
	struct msm_vidc_qc2_control input, output, sentinel;
	unsigned int i, j;
	__s32 v;

	/* Bind the expected legacy ABI to this repository's header. */
	CHECK(V4L2_CID_MPEG_VIDC_VIDEO_OUTPUT_ORDER == 0x00992003U);
	CHECK(V4L2_CID_MPEG_VIDC_VIDEO_LOWLATENCY_MODE == 0x00992038U);
	CHECK(V4L2_CID_MPEG_VIDC_VIDEO_SYNC_FRAME_DECODE == 0x0099200fU);
	CHECK(V4L2_CID_MPEG_VIDC_VIDEO_KEEP_ASPECT_RATIO == 0x00992001U);

	for (i = 0; i < sizeof(mappings) / sizeof(mappings[0]); i++) {
		for (v = 0; v <= 1; v++) {
			input.id = mappings[i].modern;
			input.value = v;
			CHECK(!msm_vidc_qc2_translate_ctrl(MSM_VIDC_QC2_SPRING,
					&input, &output));
			CHECK(output.id == mappings[i].legacy);
			CHECK(output.value == v);
		}
		for (j = 0; j < sizeof(bad_values) / sizeof(bad_values[0]); j++) {
			input.id = mappings[i].modern;
			input.value = bad_values[j];
			sentinel.id = 0xdeadbeefU;
			sentinel.value = 123;
			output = sentinel;
			CHECK(msm_vidc_qc2_translate_ctrl(MSM_VIDC_QC2_SPRING,
					&input, &output) == -EINVAL);
			CHECK(!memcmp(&output, &sentinel, sizeof(output)));
		}
	}
	for (i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); i++) {
		input.id = unsupported[i];
		input.value = 0;
		output = sentinel;
		CHECK(msm_vidc_qc2_translate_ctrl(MSM_VIDC_QC2_SPRING,
				&input, &output) == -EOPNOTSUPP);
		CHECK(!memcmp(&output, &sentinel, sizeof(output)));
		/* The same number still passes unchanged in the old namespace. */
		CHECK(!msm_vidc_qc2_translate_ctrl(MSM_VIDC_QC2_LEGACY,
				&input, &output));
		CHECK(!memcmp(&output, &input, sizeof(output)));
	}

	input.id = QC2_SPRING_LOW_LATENCY;
	input.value = 1;
	CHECK(!msm_vidc_qc2_translate_ctrl(MSM_VIDC_QC2_SPRING, &input, &input));
	CHECK(input.id == V4L2_CID_MPEG_VIDC_VIDEO_LOWLATENCY_MODE);
	CHECK(msm_vidc_qc2_translate_ctrl(99, &input, &output) == -EINVAL);
	CHECK(msm_vidc_qc2_translate_ctrl(MSM_VIDC_QC2_LEGACY, NULL, &output) == -EINVAL);
	CHECK(msm_vidc_qc2_translate_ctrl(MSM_VIDC_QC2_LEGACY, &input, NULL) == -EINVAL);
}

static void test_plane_mapping(void)
{
	struct msm_vidc_qc2_dmabuf_plane input;
	struct msm_vidc_qc2_legacy_plane output, sentinel;
	unsigned int i;
	static const struct msm_vidc_qc2_dmabuf_plane invalid[] = {
		{ -1, 4096, 128, 0 },
		{ INT_MIN, 4096, 128, 0 },
		{ 3, 0, 0, 0 },
		{ 3, 4096, 4097, 0 },
		{ 3, 4096, 128, 4097 },
		{ 3, 4096, 128, 129 },
	};

	input.fd = 0; /* fd zero is valid and must not be rejected. */
	input.length = 4096;
	input.bytesused = 1024;
	input.data_offset = 256;
	memset(&output, 0xff, sizeof(output));
	CHECK(!msm_vidc_qc2_plane_to_legacy(&input, &output));
	CHECK(output.fd == 0 && output.reserved[0] == 0);
	CHECK(output.length == 4096 && output.bytesused == 1024);
	CHECK(output.data_offset == 256 && output.reserved[1] == 256);
	for (i = 2; i < 11; i++)
		CHECK(output.reserved[i] == 0);

	input.fd = INT_MAX;
	input.length = UINT_MAX;
	input.bytesused = UINT_MAX;
	input.data_offset = UINT_MAX;
	CHECK(!msm_vidc_qc2_plane_to_legacy(&input, &output));
	CHECK(output.reserved[0] == INT_MAX && output.reserved[1] == UINT_MAX);

	/* Empty capture/EOS descriptors remain representable, without fake data. */
	input.fd = 3;
	input.length = 4096;
	input.bytesused = 0;
	input.data_offset = 0;
	CHECK(!msm_vidc_qc2_plane_to_legacy(&input, &output));
	CHECK(output.bytesused == 0 && output.fd == 3);

	memset(&sentinel, 0x7f, sizeof(sentinel));
	for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		output = sentinel;
		CHECK(msm_vidc_qc2_plane_to_legacy(&invalid[i], &output) == -EINVAL);
		CHECK(!memcmp(&output, &sentinel, sizeof(output)));
	}
	CHECK(msm_vidc_qc2_plane_to_legacy(NULL, &output) == -EINVAL);
	CHECK(msm_vidc_qc2_plane_to_legacy(&input, NULL) == -EINVAL);
}

static void test_legacy_namespace_isolation(void)
{
	struct msm_vidc_qc2_control input, output;
	__u32 seed = 12345;
	unsigned int i;

	/* Includes values and IDs that would be rejected by the modern translator. */
	for (i = 0; i < 10000; i++) {
		seed = seed * 1664525U + 1013904223U;
		input.id = seed;
		seed = seed * 1664525U + 1013904223U;
		memcpy(&input.value, &seed, sizeof(seed));
		CHECK(!msm_vidc_qc2_translate_ctrl(MSM_VIDC_QC2_LEGACY,
				&input, &output));
		CHECK(!memcmp(&input, &output, sizeof(input)));
	}
}

int main(void)
{
	test_controls();
	test_plane_mapping();
	test_legacy_namespace_isolation();
	printf("PASS: %u checks; no device or kernel transport executed\n", checks);
	return 0;
}
