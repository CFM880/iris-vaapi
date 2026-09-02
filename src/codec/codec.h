// SPDX-License-Identifier: GPL-2.0-or-later
/* VA parameter-to-access-unit codec abstraction. */

#ifndef VPU_VAAPI_CODEC_H
#define VPU_VAAPI_CODEC_H

#include <stddef.h>
#include <stdint.h>

#include <va/va.h>

#include "codec/types.h"

struct vpu_codec;

struct vpu_codec_access_unit {
	const uint8_t *data;
	size_t size;
	int random_access;
	int32_t picture_order_count;
	int refs_l0;
	int refs_l1;
	uint64_t rewrite_ns;
	uint64_t rewrite_bytes;
	uint64_t rewrites;
};

/* Profile registry shared by the VA frontend and decode scheduler. */
int vpu_codec_profile_info(VAProfile profile, enum vpu_codec_id *codec,
			    enum vpu_pixel_format *format,
			    const char **name);
unsigned int vpu_codec_profile_count(void);
VAProfile vpu_codec_profile_at(unsigned int index);

struct vpu_codec *vpu_codec_create(VAProfile profile, unsigned int width,
				     unsigned int height);
void vpu_codec_destroy(struct vpu_codec *codec);
enum vpu_codec_id vpu_codec_id(const struct vpu_codec *codec);
enum vpu_pixel_format vpu_codec_pixel_format(const struct vpu_codec *codec);
const char *vpu_codec_name(const struct vpu_codec *codec);

void vpu_codec_reconfigure(struct vpu_codec *codec, unsigned int width,
			    unsigned int height);
void vpu_codec_reset_session(struct vpu_codec *codec);
void vpu_codec_begin_picture(struct vpu_codec *codec);
void vpu_codec_finish_picture(struct vpu_codec *codec);

/* Consume a complete VA buffer.  Element count is meaningful for slice
 * parameter buffers and must be one for ordinary buffers. */
int vpu_codec_render(struct vpu_codec *codec, VABufferType type,
		      const void *data, size_t size, unsigned int elements);
int vpu_codec_build_access_unit(struct vpu_codec *codec,
				 struct vpu_codec_access_unit *unit);

/* Some stateful VPU implementations need a codec-valid internal access unit
 * to release the preceding picture.  Returns -ENOTSUP for codecs without one. */
int vpu_codec_build_release_access_unit(struct vpu_codec *codec,
					 uint8_t data[2], size_t *size);

#endif
