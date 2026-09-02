// SPDX-License-Identifier: GPL-2.0-or-later

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "codec_internal.h"

struct vpu_codec {
	const struct vpu_codec_ops *ops;
	void *private;
	enum vpu_pixel_format pixel_format;
};

static const struct vpu_codec_ops *const codecs[] = {
	&vpu_h264_codec_ops,
	&vpu_hevc_codec_ops,
	&vpu_vp9_codec_ops,
};

static const VAProfile codec_profiles[] = {
	VAProfileH264ConstrainedBaseline,
	VAProfileH264Main,
	VAProfileH264High,
	VAProfileHEVCMain,
	VAProfileHEVCMain10,
	VAProfileVP9Profile0,
	VAProfileVP9Profile2,
};

static const struct vpu_codec_ops *find_codec(VAProfile profile)
{
	size_t i;

	for (i = 0; i < sizeof(codecs) / sizeof(codecs[0]); i++)
		if (codecs[i]->supports_profile(profile))
			return codecs[i];
	return NULL;
}

int vpu_codec_profile_info(VAProfile profile, enum vpu_codec_id *codec,
			    enum vpu_pixel_format *format, const char **name)
{
	const struct vpu_codec_ops *ops = find_codec(profile);

	if (!ops)
		return -ENOTSUP;
	if (codec)
		*codec = ops->id;
	if (format)
		*format = ops->pixel_format(profile);
	if (name)
		*name = ops->name;
	return 0;
}

unsigned int vpu_codec_profile_count(void)
{
	return sizeof(codec_profiles) / sizeof(codec_profiles[0]);
}

VAProfile vpu_codec_profile_at(unsigned int index)
{
	return index < vpu_codec_profile_count() ? codec_profiles[index] :
		VAProfileNone;
}

struct vpu_codec *vpu_codec_create(VAProfile profile, unsigned int width,
				     unsigned int height)
{
	const struct vpu_codec_ops *ops = find_codec(profile);
	struct vpu_codec *codec;

	if (!ops)
		return NULL;
	codec = calloc(1, sizeof(*codec));
	if (!codec)
		return NULL;
	codec->private = ops->create(profile, width, height);
	if (!codec->private) {
		free(codec);
		return NULL;
	}
	codec->ops = ops;
	codec->pixel_format = ops->pixel_format(profile);
	return codec;
}

void vpu_codec_destroy(struct vpu_codec *codec)
{
	if (!codec)
		return;
	codec->ops->destroy(codec->private);
	free(codec);
}

enum vpu_codec_id vpu_codec_id(const struct vpu_codec *codec)
{
	return codec->ops->id;
}

enum vpu_pixel_format vpu_codec_pixel_format(const struct vpu_codec *codec)
{
	return codec->pixel_format;
}

const char *vpu_codec_name(const struct vpu_codec *codec)
{
	return codec->ops->name;
}

void vpu_codec_reconfigure(struct vpu_codec *codec, unsigned int width,
			    unsigned int height)
{
	codec->ops->reconfigure(codec->private, width, height);
}

void vpu_codec_reset_session(struct vpu_codec *codec)
{
	if (codec)
		codec->ops->reset_session(codec->private);
}

void vpu_codec_begin_picture(struct vpu_codec *codec)
{
	codec->ops->begin_picture(codec->private);
}

void vpu_codec_finish_picture(struct vpu_codec *codec)
{
	codec->ops->finish_picture(codec->private);
}

int vpu_codec_render(struct vpu_codec *codec, VABufferType type,
		      const void *data, size_t size, unsigned int elements)
{
	if (!codec || (!data && size) || !elements)
		return -EINVAL;
	return codec->ops->render(codec->private, type, data, size, elements);
}

int vpu_codec_build_access_unit(struct vpu_codec *codec,
				 struct vpu_codec_access_unit *unit)
{
	if (!codec || !unit)
		return -EINVAL;
	memset(unit, 0, sizeof(*unit));
	return codec->ops->build_access_unit(codec->private, unit);
}

int vpu_codec_build_release_access_unit(struct vpu_codec *codec,
					 uint8_t data[2], size_t *size)
{
	if (!codec || !data || !size ||
	    !codec->ops->build_release_access_unit)
		return -ENOTSUP;
	return codec->ops->build_release_access_unit(codec->private, data, size);
}

int vpu_codec_has_start_code(const uint8_t *data, size_t size)
{
	if (size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 &&
	    data[3] == 1)
		return 4;
	if (size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1)
		return 3;
	return 0;
}

int vpu_codec_append(uint8_t **buffer, size_t *length, size_t *capacity,
		      const void *data, size_t size, int annex_b)
{
	static const uint8_t start_code[4] = { 0, 0, 0, 1 };
	size_t prefix = annex_b && !vpu_codec_has_start_code(data, size) ? 4 : 0;
	size_t required;

	if (size > SIZE_MAX - *length - prefix)
		return -E2BIG;
	required = *length + prefix + size;
	if (required > *capacity) {
		size_t next = *capacity ? *capacity : 1U << 20;
		void *allocation;

		while (next < required) {
			if (next > SIZE_MAX / 2)
				return -E2BIG;
			next *= 2;
		}
		allocation = realloc(*buffer, next);
		if (!allocation)
			return -ENOMEM;
		*buffer = allocation;
		*capacity = next;
	}
	if (prefix) {
		memcpy(*buffer + *length, start_code, prefix);
		*length += prefix;
	}
	memcpy(*buffer + *length, data, size);
	*length += size;
	return 0;
}
