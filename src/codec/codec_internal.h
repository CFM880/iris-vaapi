// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef VPU_VAAPI_CODEC_INTERNAL_H
#define VPU_VAAPI_CODEC_INTERNAL_H

#include "codec.h"

struct vpu_codec_ops {
	const char *name;
	int (*supports_profile)(VAProfile profile);
	enum vpu_codec_id id;
	enum vpu_pixel_format (*pixel_format)(VAProfile profile);
	void *(*create)(VAProfile profile, unsigned int width,
			unsigned int height);
	void (*destroy)(void *private);
	void (*reconfigure)(void *private, unsigned int width,
			    unsigned int height);
	void (*reset_session)(void *private);
	void (*begin_picture)(void *private);
	void (*finish_picture)(void *private);
	int (*render)(void *private, VABufferType type, const void *data,
		      size_t size, unsigned int elements);
	int (*build_access_unit)(void *private,
				 struct vpu_codec_access_unit *unit);
	int (*build_release_access_unit)(void *private, uint8_t data[2],
					 size_t *size);
};

extern const struct vpu_codec_ops vpu_h264_codec_ops;
extern const struct vpu_codec_ops vpu_hevc_codec_ops;
extern const struct vpu_codec_ops vpu_vp9_codec_ops;

int vpu_codec_has_start_code(const uint8_t *data, size_t size);
int vpu_codec_append(uint8_t **buffer, size_t *length, size_t *capacity,
		      const void *data, size_t size, int annex_b);

#endif
