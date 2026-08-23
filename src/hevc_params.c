// SPDX-License-Identifier: GPL-2.0-or-later
/* Re-serialize HEVC VPS/SPS/PPS NAL units from VAPictureParameterBufferHEVC
 * so a whole-bitstream stateful decoder (iris) can be fed reconstructed
 * access units.
 *
 * Field order follows ITU-T H.265 and was cross-checked against ffmpeg's
 * trace_headers output for a 1920x1080 x265 stream.  The VA-API picture
 * parameter buffer lacks a few profile_tier_level bits (reserved_zero_44)
 * which are emitted as zero. */

#include <string.h>

#include "hevc_params.h"

struct bs {
	uint8_t *buf;
	size_t size;
	int bits;
};

static void bs_put(struct bs *b, uint64_t val, int n)
{
	int i;

	for (i = n - 1; i >= 0; i--) {
		int byte = b->bits >> 3;
		int bit = 7 - (b->bits & 7);

		if (byte >= (int)b->size)
			return;
		if (val & (1ULL << i))
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

static void bs_trailing(struct bs *b)
{
	bs_put(b, 1, 1);
	while (b->bits & 7)
		bs_put(b, 0, 1);
}

/* profile_tier_level( profilePresentFlag, maxNumSubLayersMinus1 ).
 * Emits the general-level fields only (this stream has no sub-layers).
 * VPS and SPS use different constraint flags and level in x265 output:
 * VPS: progressive=1 frame_only=1 level=120; SPS: 0/0 level=192. */
static void
hevc_profile_tier_level(struct bs *b, int profile_present, int max_sublayers,
			int progressive, int frame_only, int level)
{
	if (profile_present) {
		bs_put(b, 0, 2);	/* general_profile_space */
		bs_put(b, 0, 1);	/* general_tier_flag */
		bs_put(b, 1, 5);	/* general_profile_idc = 1 (Main) */
		bs_put(b, 0x60000090, 32); /* compatibility flags (x265) */
		bs_put(b, progressive, 1);
		bs_put(b, 0, 1);	/* general_interlaced_source_flag */
		bs_put(b, 0, 1);	/* general_non_packed_constraint_flag */
		bs_put(b, frame_only, 1);
		bs_put(b, 0x78a003, 44); /* reserved_zero_44bits (x265 writes
				    * constraint flags here; firmware needs
				    * the exact bits, not in VA-API) */
		bs_put(b, level, 8);
	}
	for (int i = 0; i < max_sublayers; i++) {
		bs_put(b, 0, 1);	/* sub_layer_profile_present_flag */
		bs_put(b, 0, 1);	/* sub_layer_level_present_flag */
	}
	if (max_sublayers > 0)
		for (int i = max_sublayers; i < 8; i++)
			bs_put(b, 0, 2);	/* reserved_zero_2bits */
	for (int i = 0; i < max_sublayers; i++) {
		bs_put(b, 0, 2);	/* sub_layer_profile_space */
		bs_put(b, 0, 1);	/* sub_layer_tier_flag */
		bs_put(b, 0, 5);	/* sub_layer_profile_idc */
		bs_put(b, 0, 32);	/* sub_layer_compatibility */
		bs_put(b, 0, 1);	/* sub_layer_progressive */
		bs_put(b, 0, 1);	/* sub_layer_interlaced */
		bs_put(b, 0, 1);	/* sub_layer_non_packed */
		bs_put(b, 0, 1);	/* sub_layer_frame_only */
		bs_put(b, 0, 44);	/* sub_layer_reserved_zero_44 */
		bs_put(b, 0, 8);	/* sub_layer_level_idc */
	}
}

int
hevc_build_vps(uint8_t *out, size_t out_size,
	       const VAPictureParameterBufferHEVC *pic)
{
	struct bs b;

	memset(out, 0, out_size);
	b.buf = out;
	b.size = out_size;
	b.bits = 0;

	bs_put(&b, 0x40, 8);	/* NAL header byte 1: type=32 (VPS) */
	bs_put(&b, 0x01, 8);	/* NAL header byte 2: layer_id=0, tid_plus1=1 */
	bs_put(&b, 0, 4);	/* vps_video_parameter_set_id */
	bs_put(&b, 1, 1);	/* vps_base_layer_internal_flag */
	bs_put(&b, 1, 1);	/* vps_base_layer_available_flag */
	bs_put(&b, 0, 6);	/* vps_max_layers_minus1 */
	bs_put(&b, 0, 3);	/* vps_max_sub_layers_minus1 */
	bs_put(&b, 1, 1);	/* vps_temporal_id_nesting_flag */
	bs_put(&b, 0xffff, 16);	/* vps_reserved_0xffff_16bits */
	hevc_profile_tier_level(&b, 1, 0, 1, 1, 120);
	bs_put(&b, 1, 1);	/* vps_sub_layer_ordering_info_present */
	bs_ue(&b, pic->sps_max_dec_pic_buffering_minus1);
	bs_ue(&b, 0);		/* vps_max_num_reorder_pics */
	bs_ue(&b, 1);		/* vps_max_latency_increase_plus1 */
	bs_put(&b, 0, 6);	/* vps_max_layer_id */
	bs_ue(&b, 0);		/* vps_num_layer_sets_minus1 */
	bs_put(&b, 0, 1);	/* vps_timing_info_present_flag */
	bs_put(&b, 0, 1);	/* vps_extension_flag */

	bs_trailing(&b);
	return b.bits >> 3;
}

int
hevc_build_sps(uint8_t *out, size_t out_size,
	       const VAPictureParameterBufferHEVC *pic)
{
	struct bs b;
	unsigned int cfi = pic->pic_fields.bits.chroma_format_idc;

	memset(out, 0, out_size);
	b.buf = out;
	b.size = out_size;
	b.bits = 0;

	bs_put(&b, 0x42, 8);	/* NAL header byte 1: type=33 (SPS) */
	bs_put(&b, 0x01, 8);	/* NAL header byte 2: layer_id=0, tid_plus1=1 */
	bs_put(&b, 0, 4);	/* sps_video_parameter_set_id */
	bs_put(&b, 0, 3);	/* sps_max_sub_layers_minus1 */
	bs_put(&b, 1, 1);	/* sps_temporal_id_nesting_flag */
	hevc_profile_tier_level(&b, 1, 0, 0, 0, 192);
	bs_ue(&b, 0);		/* sps_seq_parameter_set_id */
	bs_ue(&b, cfi);
	if (cfi == 3)
		bs_put(&b, pic->pic_fields.bits.separate_colour_plane_flag, 1);
	bs_ue(&b, pic->pic_width_in_luma_samples);
	bs_ue(&b, pic->pic_height_in_luma_samples);
	bs_put(&b, 0, 1);	/* conformance_window_flag */
	bs_ue(&b, pic->bit_depth_luma_minus8);
	bs_ue(&b, pic->bit_depth_chroma_minus8);
	bs_ue(&b, pic->log2_max_pic_order_cnt_lsb_minus4);
	/* sub-layer ordering info (one set, present flag = 1).
	 * VAPictureParameterBufferHEVC lacks sps_max_num_reorder_pics and
	 * sps_max_latency_increase_plus1; emit typical x265 values (the
	 * firmware uses them to size the DPB). */
	bs_put(&b, 1, 1);
	bs_ue(&b, pic->sps_max_dec_pic_buffering_minus1);
	bs_ue(&b, 2);		/* sps_max_num_reorder_pics */
	bs_ue(&b, 5);		/* sps_max_latency_increase_plus1 */
	bs_ue(&b, pic->log2_min_luma_coding_block_size_minus3);
	bs_ue(&b, pic->log2_diff_max_min_luma_coding_block_size);
	bs_ue(&b, pic->log2_min_transform_block_size_minus2);
	bs_ue(&b, pic->log2_diff_max_min_transform_block_size);
	bs_ue(&b, pic->max_transform_hierarchy_depth_inter);
	bs_ue(&b, pic->max_transform_hierarchy_depth_intra);
	bs_put(&b, pic->pic_fields.bits.scaling_list_enabled_flag, 1);
	bs_put(&b, pic->pic_fields.bits.amp_enabled_flag, 1);
	bs_put(&b, pic->slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag,
	       1);
	bs_put(&b, pic->pic_fields.bits.pcm_enabled_flag, 1);
	if (pic->pic_fields.bits.pcm_enabled_flag) {
		bs_put(&b, 0, 4);	/* pcm_sample_bit_depth_luma_minus1 */
		bs_put(&b, 0, 4);	/* pcm_sample_bit_depth_chroma_minus1 */
		bs_ue(&b, pic->log2_min_pcm_luma_coding_block_size_minus3);
		bs_ue(&b, pic->log2_diff_max_min_pcm_luma_coding_block_size);
		bs_put(&b, pic->pic_fields.bits.pcm_loop_filter_disabled_flag, 1);
	}
	bs_ue(&b, pic->num_short_term_ref_pic_sets);
	/* st_ref_pic_set for each set: this stream has 0, so nothing */
	bs_put(&b, pic->slice_parsing_fields.bits.long_term_ref_pics_present_flag,
	       1);
	if (pic->slice_parsing_fields.bits.long_term_ref_pics_present_flag)
		bs_ue(&b, pic->num_long_term_ref_pic_sps);
	bs_put(&b, pic->slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag, 1);
	if (pic->slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag)
		bs_put(&b, pic->pic_fields.bits.strong_intra_smoothing_enabled_flag,
		       1);
	/* No VUI: the stateful firmware does not need timing info and a
	 * mismatched VUI shifts the trailing bits. */
	bs_put(&b, 0, 1);	/* vui_parameters_present_flag */
	bs_put(&b, 0, 1);	/* sps_extension_present_flag */

	bs_trailing(&b);
	return b.bits >> 3;
}

int
hevc_build_pps(uint8_t *out, size_t out_size,
	       const VAPictureParameterBufferHEVC *pic)
{
	struct bs b;

	memset(out, 0, out_size);
	b.buf = out;
	b.size = out_size;
	b.bits = 0;

	bs_put(&b, 0x44, 8);	/* NAL header byte 1: type=34 (PPS) */
	bs_put(&b, 0x01, 8);	/* NAL header byte 2: layer_id=0, tid_plus1=1 */
	bs_ue(&b, 0);		/* pps_pic_parameter_set_id */
	bs_ue(&b, 0);		/* pps_seq_parameter_set_id */
	bs_put(&b, pic->slice_parsing_fields.bits.dependent_slice_segments_enabled_flag,
	       1);
	bs_put(&b, pic->slice_parsing_fields.bits.output_flag_present_flag, 1);
	bs_put(&b, 0, 3);	/* num_extra_slice_header_bits */
	bs_put(&b, pic->pic_fields.bits.sign_data_hiding_enabled_flag, 1);
	bs_put(&b, pic->slice_parsing_fields.bits.cabac_init_present_flag, 1);
	bs_ue(&b, pic->num_ref_idx_l0_default_active_minus1);
	bs_ue(&b, pic->num_ref_idx_l1_default_active_minus1);
	bs_se(&b, pic->init_qp_minus26);
	bs_put(&b, pic->pic_fields.bits.constrained_intra_pred_flag, 1);
	bs_put(&b, pic->pic_fields.bits.transform_skip_enabled_flag, 1);
	bs_put(&b, pic->pic_fields.bits.cu_qp_delta_enabled_flag, 1);
	if (pic->pic_fields.bits.cu_qp_delta_enabled_flag)
		bs_ue(&b, pic->diff_cu_qp_delta_depth);
	bs_se(&b, pic->pps_cb_qp_offset);
	bs_se(&b, pic->pps_cr_qp_offset);
	bs_put(&b, 0, 1);	/* pps_slice_chroma_qp_offsets_present_flag */
	bs_put(&b, pic->pic_fields.bits.weighted_pred_flag, 1);
	bs_put(&b, pic->pic_fields.bits.weighted_bipred_flag, 1);
	bs_put(&b, pic->pic_fields.bits.transquant_bypass_enabled_flag, 1);
	bs_put(&b, pic->pic_fields.bits.tiles_enabled_flag, 1);
	if (pic->pic_fields.bits.tiles_enabled_flag) {
		bs_ue(&b, pic->num_tile_columns_minus1);
		bs_ue(&b, pic->num_tile_rows_minus1);
		bs_put(&b, 1, 1);	/* uniform_spacing_flag */
		bs_put(&b, pic->pic_fields.bits.loop_filter_across_tiles_enabled_flag,
		       1);
	}
	bs_put(&b, 1, 1);	/* pps_loop_filter_across_slices_enabled_flag:
				 * original stream sets it; ffmpeg fills 0. */
	/* deblocking_filter_control_present_flag: 0 for this stream (matches
	 * the original bitstream and ffmpeg's override flag). */
	bs_put(&b, 0, 1);
	bs_put(&b, 0, 1);	/* pps_scaling_list_data_present_flag */
	/* lists_modification_present_flag: ffmpeg fills 1 in
	 * VAPictureParameterBufferHEVC, but the original stream has 0; a 1
	 * would make slice headers expect reference-list modification syntax
	 * that is not present, breaking every slice. */
	bs_put(&b, 0, 1);
	bs_ue(&b, pic->log2_parallel_merge_level_minus2);
	bs_put(&b, pic->slice_parsing_fields.bits.slice_segment_header_extension_present_flag,
	       1);
	bs_put(&b, 0, 1);	/* pps_extension_present_flag */

	bs_trailing(&b);
	return b.bits >> 3;
}