// SPDX-License-Identifier: GPL-2.0-or-later

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "codec/codec_internal.h"
#include "h264_params.h"

#define H264_MAX_SLICES 128

struct h264_codec {
	VAProfile profile;
	unsigned int width;
	unsigned int height;
	VAPictureParameterBufferH264 picture;
	VAIQMatrixBufferH264 iq_matrix;
	int have_picture;
	int have_iq_matrix;
	VASliceParameterBufferH264 slices[H264_MAX_SLICES];
	unsigned int slice_count;
	int refs_l0;
	int refs_l1;
	uint8_t *slice_data;
	size_t slice_length;
	size_t slice_capacity;
	uint8_t *access_unit;
	size_t access_unit_capacity;
	uint8_t last_sps[256];
	int last_sps_length;
	uint8_t last_pps[4096];
	int last_pps_length;
	int pps_refs_l0;
	int pps_refs_l1;
};

static int h264_supports_profile(VAProfile profile)
{
	return profile == VAProfileH264ConstrainedBaseline ||
	       profile == VAProfileH264Main || profile == VAProfileH264High;
}

static enum vpu_pixel_format h264_pixel_format(VAProfile profile)
{
	return VPU_PIXEL_FORMAT_NV12;
}

static void h264_reset_session(void *private)
{
	struct h264_codec *codec = private;

	codec->last_sps_length = 0;
	codec->last_pps_length = 0;
	codec->pps_refs_l0 = -1;
	codec->pps_refs_l1 = -1;
}

static void h264_begin_picture(void *private)
{
	struct h264_codec *codec = private;

	codec->have_picture = 0;
	codec->slice_length = 0;
	codec->slice_count = 0;
	codec->refs_l0 = 0;
	codec->refs_l1 = 0;
}

static void *h264_create(VAProfile profile, unsigned int width,
			 unsigned int height)
{
	struct h264_codec *codec = calloc(1, sizeof(*codec));

	if (!codec)
		return NULL;
	codec->profile = profile;
	codec->width = width;
	codec->height = height;
	h264_reset_session(codec);
	h264_begin_picture(codec);
	return codec;
}

static void h264_destroy(void *private)
{
	struct h264_codec *codec = private;

	free(codec->slice_data);
	free(codec->access_unit);
	free(codec);
}

static void h264_reconfigure(void *private, unsigned int width,
			     unsigned int height)
{
	struct h264_codec *codec = private;

	codec->width = width;
	codec->height = height;
}

static void h264_finish_picture(void *private)
{
	struct h264_codec *codec = private;

	codec->slice_length = 0;
	codec->have_picture = 0;
}

static int h264_render(void *private, VABufferType type, const void *data,
		       size_t size, unsigned int elements)
{
	struct h264_codec *codec = private;
	unsigned int i;

	switch (type) {
	case VAPictureParameterBufferType:
		if (size < sizeof(codec->picture))
			return -EINVAL;
		memcpy(&codec->picture, data, sizeof(codec->picture));
		codec->have_picture = 1;
		return 0;
	case VASliceParameterBufferType:
		if (elements > H264_MAX_SLICES - codec->slice_count ||
		    size < sizeof(codec->slices[0]) * elements)
			return -EINVAL;
		for (i = 0; i < elements; i++) {
			const VASliceParameterBufferH264 *slice =
				(const VASliceParameterBufferH264 *)data + i;

			if (slice->num_ref_idx_l0_active_minus1 > codec->refs_l0)
				codec->refs_l0 =
					slice->num_ref_idx_l0_active_minus1;
			if (slice->num_ref_idx_l1_active_minus1 > codec->refs_l1)
				codec->refs_l1 =
					slice->num_ref_idx_l1_active_minus1;
			memcpy(&codec->slices[codec->slice_count++], slice,
			       sizeof(*slice));
		}
		return 0;
	case VASliceDataBufferType:
		return vpu_codec_append(&codec->slice_data,
			&codec->slice_length, &codec->slice_capacity,
			data, size, 1);
	case VAIQMatrixBufferType:
		if (size < sizeof(codec->iq_matrix))
			return -EINVAL;
		memcpy(&codec->iq_matrix, data, sizeof(codec->iq_matrix));
		codec->have_iq_matrix = 1;
		return 0;
	default:
		return 0;
	}
}

static int h264_random_access(const struct h264_codec *codec)
{
	size_t offset = 0;

	while (offset + 3 < codec->slice_length) {
		int prefix = vpu_codec_has_start_code(codec->slice_data + offset,
						 codec->slice_length - offset);

		if (!prefix) {
			offset++;
			continue;
		}
		offset += prefix;
		if (offset < codec->slice_length &&
		    (codec->slice_data[offset] & 0x1f) == 5)
			return 1;
		while (offset < codec->slice_length &&
		       !vpu_codec_has_start_code(codec->slice_data + offset,
						 codec->slice_length - offset))
			offset++;
	}
	return 0;
}

static int h264_update_pps_defaults(struct h264_codec *codec)
{
	size_t offset = 0;
	unsigned int slice_index = 0;

	while (offset + 3 < codec->slice_length) {
		int prefix = vpu_codec_has_start_code(codec->slice_data + offset,
						 codec->slice_length - offset);
		size_t start;
		size_t end;
		int mask;
		const VASliceParameterBufferH264 *slice;

		if (!prefix) {
			offset++;
			continue;
		}
		start = offset + prefix;
		end = start;
		while (end < codec->slice_length &&
		       !vpu_codec_has_start_code(codec->slice_data + end,
						 codec->slice_length - end))
			end++;
		if (end <= start || (codec->slice_data[start] & 0x1f) < 1 ||
		    (codec->slice_data[start] & 0x1f) > 5) {
			offset = end;
			continue;
		}
		if (slice_index >= codec->slice_count)
			return -EINVAL;
		slice = &codec->slices[slice_index++];
		mask = h264_slice_default_ref_mask(codec->slice_data + start,
						  end - start,
						  &codec->picture);
		if (mask < 0)
			return mask;
		if (mask & 1)
			codec->pps_refs_l0 =
				slice->num_ref_idx_l0_active_minus1;
		if (mask & 2)
			codec->pps_refs_l1 =
				slice->num_ref_idx_l1_active_minus1;
		offset = end;
	}
	return slice_index == codec->slice_count ? 0 : -EINVAL;
}

static int h264_reserve_access_unit(struct h264_codec *codec, size_t capacity)
{
	void *allocation;

	if (capacity <= codec->access_unit_capacity)
		return 0;
	allocation = realloc(codec->access_unit, capacity);
	if (!allocation)
		return -ENOMEM;
	codec->access_unit = allocation;
	codec->access_unit_capacity = capacity;
	return 0;
}

static int h264_build_access_unit(void *private,
				  struct vpu_codec_access_unit *unit)
{
	static const uint8_t start_code[4] = { 0, 0, 0, 1 };
	struct h264_codec *codec = private;
	const uint8_t *data;
	size_t capacity;
	size_t length = 0;
	int bytes;
	int ret;

	if (!codec->have_picture || !codec->slice_length)
		return -EINVAL;
	if (codec->slice_length > SIZE_MAX - sizeof(codec->last_sps) -
	    sizeof(codec->last_pps) - 16)
		return -E2BIG;
	capacity = codec->slice_length + sizeof(codec->last_sps) +
		sizeof(codec->last_pps) + 16;
	ret = h264_reserve_access_unit(codec, capacity);
	if (ret)
		return ret;
	data = codec->access_unit;

	if (codec->pps_refs_l0 < 0)
		codec->pps_refs_l0 = codec->picture.num_ref_frames ?
			(codec->picture.num_ref_frames > 3 ? 2 :
			 codec->picture.num_ref_frames - 1) : 0;
	if (codec->pps_refs_l1 < 0)
		codec->pps_refs_l1 = 0;
	ret = h264_update_pps_defaults(codec);
	if (ret)
		return ret;

	bytes = h264_build_sps(codec->access_unit + 4,
		capacity - 4 - codec->slice_length, &codec->picture,
		codec->profile, codec->width, codec->height);
	if (bytes <= 0)
		return -EINVAL;
	if (bytes != codec->last_sps_length ||
	    memcmp(codec->access_unit + 4, codec->last_sps, bytes)) {
		memcpy(codec->access_unit, start_code, 4);
		memcpy(codec->last_sps, codec->access_unit + 4, bytes);
		codec->last_sps_length = bytes;
		length = 4 + bytes;
	}

	bytes = h264_build_pps(codec->access_unit + length + 4,
		capacity - length - 4 - codec->slice_length, &codec->picture,
		codec->have_iq_matrix ? &codec->iq_matrix : NULL,
		codec->pps_refs_l0 < 0 ? 0 : codec->pps_refs_l0,
		codec->pps_refs_l1 < 0 ? 0 : codec->pps_refs_l1);
	if (bytes <= 0)
		return -EINVAL;
	if (bytes != codec->last_pps_length ||
	    memcmp(codec->access_unit + length + 4,
		   codec->last_pps, bytes)) {
		memcpy(codec->access_unit + length, start_code, 4);
		memcpy(codec->last_pps, codec->access_unit + length + 4, bytes);
		codec->last_pps_length = bytes;
		length += 4 + bytes;
	}
	if (codec->slice_length > capacity - length)
		return -E2BIG;
	if (!length) {
		data = codec->slice_data;
		length = codec->slice_length;
	} else {
		memcpy(codec->access_unit + length, codec->slice_data,
		       codec->slice_length);
		length += codec->slice_length;
	}

	unit->data = data;
	unit->size = length;
	unit->random_access = h264_random_access(codec);
	unit->refs_l0 = codec->refs_l0;
	unit->refs_l1 = codec->refs_l1;
	return 0;
}

const struct vpu_codec_ops vpu_h264_codec_ops = {
	.name = "h264",
	.supports_profile = h264_supports_profile,
	.id = VPU_CODEC_H264,
	.pixel_format = h264_pixel_format,
	.create = h264_create,
	.destroy = h264_destroy,
	.reconfigure = h264_reconfigure,
	.reset_session = h264_reset_session,
	.begin_picture = h264_begin_picture,
	.finish_picture = h264_finish_picture,
	.render = h264_render,
	.build_access_unit = h264_build_access_unit,
};
