// SPDX-License-Identifier: GPL-2.0-or-later

#include <string.h>

#include "h264_params.h"

/* ---- RBSP bit writer ---- */

struct bs {
	uint8_t *buf;
	size_t size;
	int bits;		/* total bits written */
};

static void bs_put(struct bs *b, unsigned int val, int n)
{
	int i;

	for (i = n - 1; i >= 0; i--) {
		int byte = b->bits >> 3;
		int bit = 7 - (b->bits & 7);

		if (byte >= (int)b->size)
			return;
		if (val & (1u << i))
			b->buf[byte] |= (1u << bit);
		b->bits++;
	}
}

static void bs_ue(struct bs *b, unsigned int val)
{
	int code = val + 1;
	int n = 32 - __builtin_clz(code);

	bs_put(b, 0, n - 1);
	bs_put(b, code, n);
}

static void bs_se(struct bs *b, int val)
{
	unsigned int code = (val <= 0) ? (unsigned int)(-2 * val)
				      : (unsigned int)(2 * val - 1);

	bs_ue(b, code);
}

/* ---- profile mapping ---- */

static int profile_idc(VAProfile profile)
{
	switch (profile) {
	case VAProfileH264ConstrainedBaseline:
		return 66;
	case VAProfileH264Main:
		return 77;
	case VAProfileH264High:
	case VAProfileH264High10:
	case VAProfileH264High422:
		return 100;
	default:
		return 100;
	}
}

static int constraint_flags(VAProfile profile)
{
	return profile == VAProfileH264ConstrainedBaseline ? 0x40 : 0;
}

/* Pick a level from the coded macroblock dimensions.  Height-only selection
 * incorrectly labels wide 4K/8K streams and makes the generated SPS reject
 * otherwise valid pictures. */
static int level_for_size(unsigned int width, unsigned int height)
{
	uint64_t mb = ((uint64_t)width + 15) / 16 *
		      (((uint64_t)height + 15) / 16);

	if (mb <= 1620)
		return 30;
	if (mb <= 3600)
		return 31;
	if (mb <= 8192)
		return 40;
	if (mb <= 8704)
		return 41;
	if (mb <= 22080)
		return 51;
	return 52;
}

int h264_build_sps(uint8_t *out, size_t out_size,
		   const VAPictureParameterBufferH264 *pic, VAProfile profile)
{
	struct bs b;
	unsigned int sps_id = 0;
	unsigned int cfi = pic->seq_fields.bits.chroma_format_idc;
	int i;

	memset(out, 0, out_size);
	b.buf = out;
	b.size = out_size;
	b.bits = 0;

	bs_put(&b, 0x67, 8);	/* NAL header: forbidden=0, ref_idc=3, type=7 */
	bs_put(&b, profile_idc(profile), 8);
	bs_put(&b, constraint_flags(profile), 8);
	bs_put(&b, level_for_size((pic->picture_width_in_mbs_minus1 + 1) * 16,
				  (pic->picture_height_in_mbs_minus1 + 1) * 16), 8);
	bs_ue(&b, sps_id);
	bs_ue(&b, cfi);
	if (cfi == 3)
		bs_put(&b, pic->seq_fields.bits.residual_colour_transform_flag,
		       1);
	bs_ue(&b, pic->bit_depth_luma_minus8);
	bs_ue(&b, pic->bit_depth_chroma_minus8);
	bs_put(&b, 0, 1);	/* qpprime_y_zero_transform_bypass_flag */
	bs_put(&b, 0, 1);	/* seq_scaling_matrix_present_flag */

	bs_ue(&b, pic->seq_fields.bits.log2_max_frame_num_minus4);
	bs_ue(&b, pic->seq_fields.bits.pic_order_cnt_type);
	if (pic->seq_fields.bits.pic_order_cnt_type == 0) {
		bs_ue(&b, pic->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4);
	} else if (pic->seq_fields.bits.pic_order_cnt_type == 1) {
		bs_put(&b, pic->seq_fields.bits.delta_pic_order_always_zero_flag,
		       1);
		bs_se(&b, 0);	/* offset_for_non_ref_pic */
		bs_se(&b, 0);	/* offset_for_top_to_bottom_field */
		bs_ue(&b, 0);	/* num_ref_frames_in_pic_order_cnt_cycle */
	}
	bs_ue(&b, pic->num_ref_frames);
	bs_put(&b, pic->seq_fields.bits.gaps_in_frame_num_value_allowed_flag,
	       1);
	bs_ue(&b, pic->picture_width_in_mbs_minus1);
	bs_ue(&b, pic->picture_height_in_mbs_minus1);
	bs_put(&b, pic->seq_fields.bits.frame_mbs_only_flag, 1);
	if (!pic->seq_fields.bits.frame_mbs_only_flag)
		bs_put(&b, pic->seq_fields.bits.mb_adaptive_frame_field_flag, 1);
	bs_put(&b, pic->seq_fields.bits.direct_8x8_inference_flag, 1);
	bs_put(&b, 0, 1);	/* frame_cropping_flag */

	/* VAPictureParameterBufferH264 does not retain the original VUI, but a
	 * stateful decoder must not hold pictures for presentation reordering:
	 * Chromium already performs that reordering with its VA-picture DPB.  Its
	 * packed-H264 builder therefore emits a minimal, legal VUI bitstream
	 * restriction with max_num_reorder_frames=0 to make the decoder return a
	 * completed picture immediately.  This is especially important here
	 * because CAPTURE is copied asynchronously into the VA target surface.
	 *
	 * Do not add invented timing information.  num_units_in_tick/time_scale
	 * are fixed u(32), not Exp-Golomb fields; the old encoding shifted the
	 * remainder of the SPS and made the following PPS invalid. */
	bs_put(&b, 1, 1);	/* vui_parameters_present_flag */
	bs_put(&b, 0, 1);	/* aspect_ratio_info_present_flag */
	bs_put(&b, 0, 1);	/* overscan_info_present_flag */
	bs_put(&b, 0, 1);	/* video_signal_type_present_flag */
	bs_put(&b, 0, 1);	/* chroma_loc_info_present_flag */
	bs_put(&b, 0, 1);	/* timing_info_present_flag */
	bs_put(&b, 0, 1);	/* nal_hrd_parameters_present_flag */
	bs_put(&b, 0, 1);	/* vcl_hrd_parameters_present_flag */
	bs_put(&b, 0, 1);	/* pic_struct_present_flag */
	bs_put(&b, 1, 1);	/* bitstream_restriction_flag */
	bs_put(&b, 0, 1);	/* motion_vectors_over_pic_boundaries_flag */
	bs_ue(&b, 2);		/* max_bytes_per_pic_denom */
	bs_ue(&b, 1);		/* max_bits_per_mb_denom */
	bs_ue(&b, 16);		/* log2_max_mv_length_horizontal */
	bs_ue(&b, 16);		/* log2_max_mv_length_vertical */
	bs_ue(&b, 0);		/* max_num_reorder_frames */
	bs_ue(&b, pic->num_ref_frames); /* max_dec_frame_buffering */

	/* rbsp_stop_one_bit + alignment */
	bs_put(&b, 1, 1);
	while (b.bits & 7)
		bs_put(&b, 0, 1);

	(void)i;
	return b.bits >> 3;
}

int h264_build_pps(uint8_t *out, size_t out_size,
		   const VAPictureParameterBufferH264 *pic,
		   int num_ref_idx_l0_default_active_minus1,
		   int num_ref_idx_l1_default_active_minus1)
{
	struct bs b;
	unsigned int sps_id = 0;
	unsigned int pps_id = 0;
	unsigned int cfi = pic->seq_fields.bits.chroma_format_idc;

	memset(out, 0, out_size);
	b.buf = out;
	b.size = out_size;
	b.bits = 0;

	bs_put(&b, 0x68, 8);	/* NAL header: forbidden=0, ref_idc=3, type=8 */
	bs_ue(&b, pps_id);
	bs_ue(&b, sps_id);
	bs_put(&b, pic->pic_fields.bits.entropy_coding_mode_flag, 1);
	bs_put(&b, pic->pic_fields.bits.pic_order_present_flag, 1);
	bs_ue(&b, 0);		/* num_slice_groups_minus1 */
	bs_ue(&b, num_ref_idx_l0_default_active_minus1);
	bs_ue(&b, num_ref_idx_l1_default_active_minus1);
	bs_put(&b, pic->pic_fields.bits.weighted_pred_flag, 1);
	bs_put(&b, pic->pic_fields.bits.weighted_bipred_idc, 2);
	bs_se(&b, pic->pic_init_qp_minus26);
	bs_se(&b, pic->pic_init_qs_minus26);
	bs_se(&b, pic->chroma_qp_index_offset);
	bs_put(&b, pic->pic_fields.bits.deblocking_filter_control_present_flag,
	       1);
	bs_put(&b, pic->pic_fields.bits.constrained_intra_pred_flag, 1);
	bs_put(&b, pic->pic_fields.bits.redundant_pic_cnt_present_flag, 1);
	if (cfi == 1)
		bs_put(&b, pic->pic_fields.bits.transform_8x8_mode_flag, 1);
	bs_put(&b, 0, 1);	/* pic_scaling_matrix_present_flag */
	bs_se(&b, pic->second_chroma_qp_index_offset);

	bs_put(&b, 1, 1);
	while (b.bits & 7)
		bs_put(&b, 0, 1);

	return b.bits >> 3;
}
