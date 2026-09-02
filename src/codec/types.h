// SPDX-License-Identifier: GPL-2.0-or-later
/* Codec identifiers and output formats shared by internal driver layers. */

#ifndef VPU_VAAPI_CODEC_TYPES_H
#define VPU_VAAPI_CODEC_TYPES_H

#include <stdint.h>

enum vpu_codec_id {
	VPU_CODEC_H264,
	VPU_CODEC_HEVC,
	VPU_CODEC_VP9,
};

/* Standard linear image FOURCC values, independent of any platform API. */
enum vpu_pixel_format {
	VPU_PIXEL_FORMAT_NV12 = 0x3231564eU,
	VPU_PIXEL_FORMAT_P010 = 0x30313050U,
};

#endif
