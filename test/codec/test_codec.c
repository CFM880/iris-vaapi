// SPDX-License-Identifier: GPL-2.0-or-later

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "codec/codec.h"

static void test_registry(void)
{
	enum vpu_codec_id id;
	enum vpu_pixel_format format;

	assert(vpu_codec_profile_count() == 7);
	assert(vpu_codec_profile_info(VAProfileH264High, &id, &format,
				       NULL) == 0);
	assert(id == VPU_CODEC_H264 && format == VPU_PIXEL_FORMAT_NV12);
	assert(vpu_codec_profile_info(VAProfileHEVCMain10, &id, &format,
				       NULL) == 0);
	assert(id == VPU_CODEC_HEVC && format == VPU_PIXEL_FORMAT_P010);
	assert(vpu_codec_profile_info(VAProfileVP9Profile2, &id, &format,
				       NULL) == 0);
	assert(id == VPU_CODEC_VP9 && format == VPU_PIXEL_FORMAT_P010);
	assert(vpu_codec_profile_info(VAProfileNone, NULL, NULL, NULL) ==
	       -ENOTSUP);
}

static void test_vp9(void)
{
	static const uint8_t key_frame[] = { 0x82, 0x00, 0x00 };
	struct vpu_codec_access_unit unit;
	struct vpu_codec *codec;
	uint8_t release[2];
	size_t release_size = 0;

	codec = vpu_codec_create(VAProfileVP9Profile0, 320, 240);
	assert(codec);
	vpu_codec_begin_picture(codec);
	assert(vpu_codec_render(codec, VASliceDataBufferType, key_frame,
				 sizeof(key_frame), 1) == 0);
	assert(vpu_codec_build_access_unit(codec, &unit) == 0);
	assert(unit.size == sizeof(key_frame));
	assert(!memcmp(unit.data, key_frame, sizeof(key_frame)));
	assert(unit.random_access);
	assert(vpu_codec_build_release_access_unit(codec, release,
						    &release_size) == 0);
	assert(release_size == 1);
	vpu_codec_finish_picture(codec);
	vpu_codec_destroy(codec);
}

static void test_h264_dispatch(void)
{
	VAPictureParameterBufferH264 picture;
	struct vpu_codec *codec;

	memset(&picture, 0, sizeof(picture));
	picture.seq_fields.bits.chroma_format_idc = 1;
	picture.seq_fields.bits.frame_mbs_only_flag = 1;
	picture.picture_width_in_mbs_minus1 = 19;
	picture.picture_height_in_mbs_minus1 = 14;
	codec = vpu_codec_create(VAProfileH264High, 320, 240);
	assert(codec);
	assert(vpu_codec_render(codec, VAPictureParameterBufferType, &picture,
				 sizeof(picture) - 1, 1) == -EINVAL);
	assert(vpu_codec_render(codec, VAPictureParameterBufferType, &picture,
				 sizeof(picture), 1) == 0);
	vpu_codec_destroy(codec);
}

static void test_hevc_access_unit(void)
{
	static const uint8_t irap[] = { 0, 0, 1, 0x26, 0x01, 0x80 };
	VAPictureParameterBufferHEVC picture;
	struct vpu_codec_access_unit unit;
	struct vpu_codec *codec;

	memset(&picture, 0, sizeof(picture));
	picture.pic_width_in_luma_samples = 320;
	picture.pic_height_in_luma_samples = 240;
	picture.pic_fields.bits.chroma_format_idc = 1;
	codec = vpu_codec_create(VAProfileHEVCMain, 320, 240);
	assert(codec);
	assert(vpu_codec_render(codec, VAPictureParameterBufferType, &picture,
				 sizeof(picture), 1) == 0);
	assert(vpu_codec_render(codec, VASliceDataBufferType, irap,
				 sizeof(irap), 1) == 0);
	assert(vpu_codec_build_access_unit(codec, &unit) == 0);
	assert(unit.size > sizeof(irap));
	assert(unit.random_access);
	vpu_codec_destroy(codec);
}

int main(void)
{
	test_registry();
	test_vp9();
	test_h264_dispatch();
	test_hevc_access_unit();
	puts("codec abstraction: ok");
	return 0;
}
