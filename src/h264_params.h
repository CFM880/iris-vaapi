// SPDX-License-Identifier: GPL-2.0-or-later
/* Re-serialize H.264 SPS/PPS NAL units from VAPictureParameterBufferH264 so a
 * whole-bitstream stateful decoder (iris) can be fed reconstructed access
 * units. */

#ifndef IRIS_VAAPI_H264_PARAMS_H
#define IRIS_VAAPI_H264_PARAMS_H

#include <stdint.h>
#include <va/va.h>

/* Build an SPS NAL unit (without start code) from the picture parameter
 * buffer.  profile_idc is taken from the VA profile.  Returns the NAL size. */
int h264_build_sps(uint8_t *out, size_t out_size,
		   const VAPictureParameterBufferH264 *pic, VAProfile profile);

/* Build a PPS NAL unit (without start code).  Returns the NAL size. */
int h264_build_pps(uint8_t *out, size_t out_size,
		   const VAPictureParameterBufferH264 *pic);

#endif