// SPDX-License-Identifier: GPL-2.0-or-later

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "codec/codec_internal.h"
#include "hevc_params.h"
#include "hevc_slice_rewrite.h"

#define HEVC_MAX_SLICES 128

struct hevc_codec {
	VAPictureParameterBufferHEVC picture;
	VAIQMatrixBufferHEVC iq_matrix;
	int have_picture;
	int have_iq_matrix;
	VASliceParameterBufferHEVC slices[HEVC_MAX_SLICES];
	unsigned int slice_count;
	unsigned int slice_next;
	int refs_l0;
	int refs_l1;
	uint8_t *slice_data;
	size_t slice_length;
	size_t slice_capacity;
	uint8_t *access_unit;
	size_t access_unit_capacity;
	uint8_t last_vps[64];
	int last_vps_length;
	uint8_t last_sps[8192];
	int last_sps_length;
	uint8_t last_pps[128];
	int last_pps_length;
	uint8_t raw_vps[1024];
	int raw_vps_length;
	uint8_t raw_sps[1024];
	int raw_sps_length;
	uint8_t raw_pps[1024];
	int raw_pps_length;
	int pps_id;
	int rewritten;
	uint64_t rewrite_ns;
	uint64_t rewrite_bytes;
	uint64_t rewrites;
	int stats_enabled;
};

static uint64_t hevc_monotonic_ns(void)
{
	struct timespec time;

	clock_gettime(CLOCK_MONOTONIC, &time);
	return (uint64_t)time.tv_sec * 1000000000ULL + time.tv_nsec;
}

static int hevc_supports_profile(VAProfile profile)
{
	return profile == VAProfileHEVCMain || profile == VAProfileHEVCMain10;
}

static enum vpu_pixel_format hevc_pixel_format(VAProfile profile)
{
	return profile == VAProfileHEVCMain10 ? VPU_PIXEL_FORMAT_P010 :
		VPU_PIXEL_FORMAT_NV12;
}

static void hevc_reset_session(void *private)
{
	struct hevc_codec *codec = private;

	codec->last_vps_length = 0;
	codec->last_sps_length = 0;
	codec->last_pps_length = 0;
	codec->raw_vps_length = 0;
	codec->raw_sps_length = 0;
	codec->raw_pps_length = 0;
}

static void hevc_begin_picture(void *private)
{
	struct hevc_codec *codec = private;

	codec->have_picture = 0;
	codec->slice_length = 0;
	codec->slice_count = 0;
	codec->slice_next = 0;
	codec->refs_l0 = 0;
	codec->refs_l1 = 0;
	codec->pps_id = -1;
	codec->rewritten = 0;
	codec->rewrite_ns = 0;
	codec->rewrite_bytes = 0;
	codec->rewrites = 0;
}

static void *hevc_create(VAProfile profile, unsigned int width,
			 unsigned int height)
{
	struct hevc_codec *codec = calloc(1, sizeof(*codec));

	if (!codec)
		return NULL;
	codec->stats_enabled = getenv("VPU_VAAPI_STATS") != NULL;
	hevc_reset_session(codec);
	hevc_begin_picture(codec);
	return codec;
}

static void hevc_destroy(void *private)
{
	struct hevc_codec *codec = private;

	free(codec->slice_data);
	free(codec->access_unit);
	free(codec);
}

static void hevc_reconfigure(void *private, unsigned int width,
			     unsigned int height)
{
}

static void hevc_finish_picture(void *private)
{
	struct hevc_codec *codec = private;

	codec->slice_length = 0;
	codec->have_picture = 0;
}

static int hevc_append_rewritten_slices(struct hevc_codec *codec,
					const void *data, size_t size)
{
	unsigned int remaining = codec->slice_count - codec->slice_next;
	unsigned int use = 0;
	unsigned int i;

	if (remaining > 1) {
		int all_fit = 1;

		for (i = 0; i < remaining; i++) {
			const VASliceParameterBufferHEVC *slice =
				&codec->slices[codec->slice_next + i];

			if ((uint64_t)slice->slice_data_offset +
			    slice->slice_data_size > size) {
				all_fit = 0;
				break;
			}
		}
		if (all_fit)
			use = remaining;
	}
	if (!use)
		use = 1;

	for (i = 0; i < use; i++) {
		static const uint8_t start_code[4] = { 0, 0, 0, 1 };
		const VASliceParameterBufferHEVC *slice =
			&codec->slices[codec->slice_next + i];
		const uint8_t *source;
		uint8_t *rewritten;
		size_t nal_length;
		size_t rewritten_capacity;
		size_t required;
		unsigned int pps_id;
		int prefix;
		int rewritten_length;
		uint64_t start;

		if ((uint64_t)slice->slice_data_offset +
		    slice->slice_data_size > size || !codec->have_picture)
			return -EINVAL;
		source = (const uint8_t *)data + slice->slice_data_offset;
		prefix = vpu_codec_has_start_code(source,
						 slice->slice_data_size);
		nal_length = slice->slice_data_size - prefix;
		if (nal_length > (SIZE_MAX - 1024) / 2)
			return -E2BIG;
		rewritten_capacity = nal_length * 2 + 1024;
		if (rewritten_capacity > SIZE_MAX - codec->slice_length - 4)
			return -E2BIG;
		required = codec->slice_length + 4 + rewritten_capacity;
		if (required > codec->slice_capacity) {
			size_t next = codec->slice_capacity ?
				codec->slice_capacity : 1U << 20;
			void *allocation;

			while (next < required) {
				if (next > SIZE_MAX / 2)
					return -E2BIG;
				next *= 2;
			}
			allocation = realloc(codec->slice_data, next);
			if (!allocation)
				return -ENOMEM;
			codec->slice_data = allocation;
			codec->slice_capacity = next;
		}
		rewritten = codec->slice_data + codec->slice_length + 4;
		start = codec->stats_enabled ? hevc_monotonic_ns() : 0;
		rewritten_length = hevc_rewrite_slice(rewritten,
			rewritten_capacity, source + prefix, nal_length,
			&codec->picture, slice, &pps_id);
		if (codec->stats_enabled) {
			codec->rewrite_ns += hevc_monotonic_ns() - start;
			codec->rewrite_bytes += nal_length;
			codec->rewrites++;
		}
		if (rewritten_length < 0) {
			fprintf(stderr, "HEVC slice rewrite failed: %d\n",
				rewritten_length);
			return rewritten_length;
		}
		if (codec->pps_id < 0)
			codec->pps_id = (int)pps_id;
		else if (codec->pps_id != (int)pps_id)
			return -EINVAL;
		if ((size_t)rewritten_length != nal_length ||
		    memcmp(rewritten, source + prefix, nal_length))
			codec->rewritten = 1;
		memcpy(codec->slice_data + codec->slice_length, start_code, 4);
		codec->slice_length += 4 + rewritten_length;
	}
	codec->slice_next += use;
	return 0;
}

static int hevc_render(void *private, VABufferType type, const void *data,
		       size_t size, unsigned int elements)
{
	struct hevc_codec *codec = private;
	unsigned int i;

	switch (type) {
	case VAPictureParameterBufferType:
		if (size < sizeof(codec->picture))
			return -EINVAL;
		memcpy(&codec->picture, data, sizeof(codec->picture));
		codec->have_picture = 1;
		return 0;
	case VASliceParameterBufferType:
		if (elements > HEVC_MAX_SLICES - codec->slice_count ||
		    size < sizeof(codec->slices[0]) * elements)
			return -EINVAL;
		for (i = 0; i < elements; i++) {
			const VASliceParameterBufferHEVC *slice =
				(const VASliceParameterBufferHEVC *)data + i;

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
		if (codec->slice_next < codec->slice_count)
			return hevc_append_rewritten_slices(codec, data, size);
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

static int hevc_random_access(const struct hevc_codec *codec)
{
	size_t offset = 0;

	while (offset + 3 < codec->slice_length) {
		int prefix = vpu_codec_has_start_code(codec->slice_data + offset,
						 codec->slice_length - offset);
		unsigned int type;

		if (!prefix) {
			offset++;
			continue;
		}
		offset += prefix;
		if (offset >= codec->slice_length)
			break;
		type = (codec->slice_data[offset] >> 1) & 0x3f;
		if (type >= 16 && type <= 23)
			return 1;
		while (offset < codec->slice_length &&
		       !vpu_codec_has_start_code(codec->slice_data + offset,
						 codec->slice_length - offset))
			offset++;
	}
	return 0;
}

static int hevc_cache_raw_parameter_sets(struct hevc_codec *codec)
{
	size_t offset = 0;
	int found = 0;

	while (offset + 3 < codec->slice_length) {
		int prefix = vpu_codec_has_start_code(codec->slice_data + offset,
						 codec->slice_length - offset);
		size_t start;
		size_t end;
		uint8_t type;

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
		if (end <= start + 1) {
			offset = end;
			continue;
		}
		type = (codec->slice_data[start] >> 1) & 0x3f;
		if (type == 32 && end - start <= sizeof(codec->raw_vps)) {
			memcpy(codec->raw_vps, codec->slice_data + start,
			       end - start);
			codec->raw_vps_length = end - start;
			found |= 1;
		} else if (type == 33 && end - start <= sizeof(codec->raw_sps)) {
			memcpy(codec->raw_sps, codec->slice_data + start,
			       end - start);
			codec->raw_sps_length = end - start;
			found |= 2;
		} else if (type == 34 && end - start <= sizeof(codec->raw_pps)) {
			memcpy(codec->raw_pps, codec->slice_data + start,
			       end - start);
			codec->raw_pps_length = end - start;
			found |= 4;
		}
		offset = end;
	}
	return found;
}

static int hevc_reserve_access_unit(struct hevc_codec *codec, size_t capacity)
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

static int hevc_build_access_unit(void *private,
				  struct vpu_codec_access_unit *unit)
{
	static const uint8_t start_code[4] = { 0, 0, 0, 1 };
	struct hevc_codec *codec = private;
	size_t capacity;
	size_t length = 0;
	int raw_in_unit;
	int bytes;
	int ret;

	if (!codec->have_picture || !codec->slice_length ||
	    codec->slice_next != codec->slice_count)
		return -EINVAL;
	if (codec->slice_length > SIZE_MAX - sizeof(codec->raw_vps) -
	    sizeof(codec->raw_sps) - sizeof(codec->raw_pps) -
	    sizeof(codec->last_sps) - 32)
		return -E2BIG;
	capacity = codec->slice_length + sizeof(codec->raw_vps) +
		sizeof(codec->raw_sps) + sizeof(codec->raw_pps) +
		sizeof(codec->last_sps) + 32;
	ret = hevc_reserve_access_unit(codec, capacity);
	if (ret)
		return ret;

	raw_in_unit = codec->rewritten ? 0 :
		hevc_cache_raw_parameter_sets(codec);
	if (raw_in_unit == 7 && !codec->rewritten) {
		memcpy(codec->access_unit, codec->slice_data,
		       codec->slice_length);
		length = codec->slice_length;
		goto complete;
	}
	if (!codec->rewritten && codec->raw_vps_length &&
	    codec->raw_sps_length && codec->raw_pps_length) {
		memcpy(codec->access_unit + length, start_code, 4);
		memcpy(codec->access_unit + length + 4, codec->raw_vps,
		       codec->raw_vps_length);
		length += 4 + codec->raw_vps_length;
		memcpy(codec->access_unit + length, start_code, 4);
		memcpy(codec->access_unit + length + 4, codec->raw_sps,
		       codec->raw_sps_length);
		length += 4 + codec->raw_sps_length;
		memcpy(codec->access_unit + length, start_code, 4);
		memcpy(codec->access_unit + length + 4, codec->raw_pps,
		       codec->raw_pps_length);
		length += 4 + codec->raw_pps_length;
		memcpy(codec->access_unit + length, codec->slice_data,
		       codec->slice_length);
		length += codec->slice_length;
		goto complete;
	}

	bytes = hevc_build_vps(codec->access_unit + 4,
		capacity - 4 - codec->slice_length, &codec->picture);
	if (bytes <= 0)
		return -EINVAL;
	if (bytes != codec->last_vps_length ||
	    memcmp(codec->access_unit + 4, codec->last_vps, bytes)) {
		memcpy(codec->access_unit, start_code, 4);
		memcpy(codec->last_vps, codec->access_unit + 4, bytes);
		codec->last_vps_length = bytes;
		length = 4 + bytes;
	}
	bytes = hevc_build_sps(codec->access_unit + length + 4,
		capacity - length - 4 - codec->slice_length, &codec->picture,
		codec->have_iq_matrix ? &codec->iq_matrix : NULL);
	if (bytes <= 0)
		return -EINVAL;
	if (bytes != codec->last_sps_length ||
	    memcmp(codec->access_unit + length + 4,
		   codec->last_sps, bytes)) {
		memcpy(codec->access_unit + length, start_code, 4);
		memcpy(codec->last_sps, codec->access_unit + length + 4,
		       bytes);
		codec->last_sps_length = bytes;
		length += 4 + bytes;
	}
	bytes = hevc_build_pps_id(codec->access_unit + length + 4,
		capacity - length - 4 - codec->slice_length, &codec->picture,
		codec->pps_id < 0 ? 0 : (unsigned int)codec->pps_id);
	if (bytes <= 0)
		return -EINVAL;
	if (bytes != codec->last_pps_length ||
	    memcmp(codec->access_unit + length + 4,
		   codec->last_pps, bytes)) {
		memcpy(codec->access_unit + length, start_code, 4);
		memcpy(codec->last_pps, codec->access_unit + length + 4,
		       bytes);
		codec->last_pps_length = bytes;
		length += 4 + bytes;
	}
	if (codec->slice_length > capacity - length)
		return -E2BIG;
	memcpy(codec->access_unit + length, codec->slice_data,
	       codec->slice_length);
	length += codec->slice_length;

complete:
	unit->data = codec->access_unit;
	unit->size = length;
	unit->random_access = hevc_random_access(codec);
	unit->picture_order_count = codec->picture.CurrPic.pic_order_cnt;
	unit->refs_l0 = codec->refs_l0;
	unit->refs_l1 = codec->refs_l1;
	unit->rewrite_ns = codec->rewrite_ns;
	unit->rewrite_bytes = codec->rewrite_bytes;
	unit->rewrites = codec->rewrites;
	return 0;
}

const struct vpu_codec_ops vpu_hevc_codec_ops = {
	.name = "hevc",
	.supports_profile = hevc_supports_profile,
	.id = VPU_CODEC_HEVC,
	.pixel_format = hevc_pixel_format,
	.create = hevc_create,
	.destroy = hevc_destroy,
	.reconfigure = hevc_reconfigure,
	.reset_session = hevc_reset_session,
	.begin_picture = hevc_begin_picture,
	.finish_picture = hevc_finish_picture,
	.render = hevc_render,
	.build_access_unit = hevc_build_access_unit,
};
