// SPDX-License-Identifier: GPL-2.0-or-later

#include <errno.h>
#include <stdlib.h>

#include "codec/codec_internal.h"

struct vp9_codec {
	uint8_t *frame_data;
	size_t frame_length;
	size_t frame_capacity;
};

static int vp9_supports_profile(VAProfile profile)
{
	return profile == VAProfileVP9Profile0 || profile == VAProfileVP9Profile2;
}

static enum vpu_pixel_format vp9_pixel_format(VAProfile profile)
{
	return profile == VAProfileVP9Profile2 ? VPU_PIXEL_FORMAT_P010 :
		VPU_PIXEL_FORMAT_NV12;
}

static void *vp9_create(VAProfile profile, unsigned int width,
			unsigned int height)
{
	return calloc(1, sizeof(struct vp9_codec));
}

static void vp9_destroy(void *private)
{
	struct vp9_codec *codec = private;

	free(codec->frame_data);
	free(codec);
}

static void vp9_reconfigure(void *private, unsigned int width,
			    unsigned int height)
{
}

static void vp9_reset_session(void *private)
{
}

static void vp9_begin_picture(void *private)
{
	((struct vp9_codec *)private)->frame_length = 0;
}

static void vp9_finish_picture(void *private)
{
	((struct vp9_codec *)private)->frame_length = 0;
}

static int vp9_render(void *private, VABufferType type, const void *data,
		      size_t size, unsigned int elements)
{
	struct vp9_codec *codec = private;

	/* VP9 picture and slice parameter buffers duplicate information already
	 * carried in the uncompressed frame header. */
	if (type != VASliceDataBufferType)
		return 0;
	return vpu_codec_append(&codec->frame_data, &codec->frame_length,
		&codec->frame_capacity, data, size, 0);
}

static int vp9_random_access(const struct vp9_codec *codec)
{
	unsigned int profile;
	unsigned int show_existing;
	uint8_t byte;

	if (!codec->frame_length)
		return 0;
	byte = codec->frame_data[0];
	if ((byte >> 6) != 2)
		return 0;
	profile = ((byte >> 5) & 1) | (((byte >> 4) & 1) << 1);
	show_existing = (byte >> (profile == 3 ? 2 : 3)) & 1;
	if (show_existing)
		return 0;
	return ((byte >> (profile == 3 ? 1 : 2)) & 1) == 0;
}

static int vp9_build_access_unit(void *private,
				 struct vpu_codec_access_unit *unit)
{
	struct vp9_codec *codec = private;

	if (!codec->frame_length)
		return -EINVAL;
	unit->data = codec->frame_data;
	unit->size = codec->frame_length;
	unit->random_access = vp9_random_access(codec);
	return 0;
}

static int vp9_build_release_access_unit(void *private, uint8_t data[2],
					 size_t *size)
{
	struct vp9_codec *codec = private;
	unsigned int profile;
	uint8_t byte;

	if (!codec->frame_length)
		return -EINVAL;
	byte = codec->frame_data[0];
	if ((byte >> 6) != 2)
		return -EINVAL;
	profile = ((byte >> 5) & 1) | (((byte >> 4) & 1) << 1);
	data[0] = 0x80 | (byte & 0x30);
	if (profile == 3) {
		data[0] |= 0x04;
		data[1] = 0;
		*size = 2;
	} else {
		data[0] |= 0x08;
		*size = 1;
	}
	return 0;
}

const struct vpu_codec_ops vpu_vp9_codec_ops = {
	.name = "vp9",
	.supports_profile = vp9_supports_profile,
	.id = VPU_CODEC_VP9,
	.pixel_format = vp9_pixel_format,
	.create = vp9_create,
	.destroy = vp9_destroy,
	.reconfigure = vp9_reconfigure,
	.reset_session = vp9_reset_session,
	.begin_picture = vp9_begin_picture,
	.finish_picture = vp9_finish_picture,
	.render = vp9_render,
	.build_access_unit = vp9_build_access_unit,
	.build_release_access_unit = vp9_build_release_access_unit,
};
