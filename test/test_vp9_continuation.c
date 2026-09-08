// SPDX-License-Identifier: GPL-2.0-or-later
/* Exercise reference ownership and strided surface copies without a VPU. */
#include <assert.h>
#include "../src/decode.c"

void vpu_platform_session_capture_layout(const struct vpu_platform_session *session,
	unsigned int *pitch, unsigned int *width, unsigned int *height)
{
	*pitch = 1280;
	*width = 1280;
	*height = 736;
}

int vpu_platform_session_signal_surface_fence(struct vpu_platform_session *session,
	uint64_t token)
{
	return 0;
}

int main(void)
{
	struct vpu_surfaces surfaces = {0};
	struct vpu_decode_ctx old = { .codec = VPU_CODEC_VP9,
		.dec_started = 1, .dec_open = 1, .width = 1280, .height = 720,
		.pixel_format = VPU_PIXEL_FORMAT_NV12, .surfs = &surfaces };
	struct vpu_decode_ctx next = { .codec = VPU_CODEC_VP9,
		.width = 640, .height = 360, .pixel_format = VPU_PIXEL_FORMAT_NV12,
		.surfs = &surfaces };
	VADecPictureParameterBufferVP9 pic = {0};
	unsigned int pitch, width, height;
	size_t src_size = 1280 * 736 * 3 / 2;
	size_t dst_size = 640 * 384 * 3 / 2;
	uint8_t *src = malloc(src_size), *dst = malloc(dst_size + 16);
	struct vpu_surface target = { .sw = 640, .sh = 360,
		.fourcc = VPU_PIXEL_FORMAT_NV12, .owner = &old,
		.bsize = dst_size, .bmap = dst };

	assert(src && dst);
	surfaces.n = 2;
	surfaces.s[0].id = 10;
	surfaces.s[1].id = 11;
	surfaces.s[0].owner = surfaces.s[1].owner = &old;
	pic.reference_frames[0] = 10;
	pic.reference_frames[1] = 11;
	pic.pic_fields.bits.frame_type = 1;
	pic.pic_fields.bits.golden_ref_frame = 1;
	assert(vpu_decode_vp9_predecessor(&next, &pic) == &old);
	/* Never continue from mixed owners, absent references, or a keyframe. */
	surfaces.s[1].owner = &next;
	assert(!vpu_decode_vp9_predecessor(&next, &pic));
	surfaces.s[1].owner = &old;
	pic.reference_frames[1] = VA_INVALID_SURFACE;
	assert(!vpu_decode_vp9_predecessor(&next, &pic));
	pic.reference_frames[1] = 11;
	pic.pic_fields.bits.frame_type = 0;
	assert(!vpu_decode_vp9_predecessor(&next, &pic));
	pic.pic_fields.bits.frame_type = 1;
	next.dec_open = 1;
	assert(!vpu_decode_vp9_predecessor(&next, &pic));
	next.dec_open = 0;
	old.eos_sent = 1;
	assert(!vpu_decode_retain_vp9(&old));
	old.eos_sent = 0;
	surfaces.n = 0;
	assert(!vpu_decode_retain_vp9(&old));

	/* Distinct row patterns expose both luma stride and UV offset errors. */
	for (unsigned int y = 0; y < 736 * 3 / 2; y++)
		memset(src + y * 1280, y % 251, 1280);
	memset(dst, 0xa5, dst_size + 16);
	target.bfd = open("/dev/null", O_RDWR);
	assert(target.bfd >= 0);
	assert(surface_copy(&old, &target, src, src_size) == 0);
	surface_layout(&target, &pitch, &width, &height);
	assert(pitch == 640 && width == 640 && height == 384);
	for (unsigned int y = 0; y < 360; y++)
		assert(!memcmp(dst + y * 640, src + y * 1280, 640));
	for (unsigned int y = 0; y < 180; y++)
		assert(!memcmp(dst + 640 * 384 + y * 640,
			src + 1280 * 736 + y * 1280, 640));
	for (size_t i = dst_size; i < dst_size + 16; i++)
		assert(dst[i] == 0xa5);
	assert(surface_copy(&old, &target, src, 1280 * 736) == -EINVAL);
	close(target.bfd);
	free(src);
	free(dst);
	puts("VP9 reference continuation and rectangular copy: ok");
	return 0;
}
