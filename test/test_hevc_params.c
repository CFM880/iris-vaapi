// SPDX-License-Identifier: GPL-2.0-or-later
/* Validate HEVC VPS/SPS/PPS re-serialization: reconstruct parameter sets from
 * a real VAPictureParameterBufferHEVC (values matching the 1920x1080 x265
 * test stream) and dump the NAL bytes for comparison against the original.
 */

#include <stdio.h>
#include <string.h>
#include <va/va.h>
#include "hevc_params.h"

int main(void)
{
	VAPictureParameterBufferHEVC pic;
	uint8_t out[512];
	int n;

	memset(&pic, 0, sizeof(pic));
	pic.pic_width_in_luma_samples = 1920;
	pic.pic_height_in_luma_samples = 1080;
	pic.pic_fields.bits.chroma_format_idc = 1;
	pic.bit_depth_luma_minus8 = 0;
	pic.bit_depth_chroma_minus8 = 0;
	pic.log2_max_pic_order_cnt_lsb_minus4 = 4;
	pic.sps_max_dec_pic_buffering_minus1 = 4;
	pic.log2_min_luma_coding_block_size_minus3 = 0;
	pic.log2_diff_max_min_luma_coding_block_size = 3;
	pic.log2_min_transform_block_size_minus2 = 0;
	pic.log2_diff_max_min_transform_block_size = 3;
	pic.max_transform_hierarchy_depth_intra = 0;
	pic.max_transform_hierarchy_depth_inter = 0;
	pic.slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag = 1;
	pic.slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag = 1;
	pic.pic_fields.bits.strong_intra_smoothing_enabled_flag = 1;
	pic.pic_fields.bits.pcm_enabled_flag = 0;
	pic.num_short_term_ref_pic_sets = 0;

	/* PPS fields matching the original stream. */
	pic.pic_fields.bits.sign_data_hiding_enabled_flag = 1;
	pic.pic_fields.bits.cu_qp_delta_enabled_flag = 1;
	pic.diff_cu_qp_delta_depth = 1;
	pic.pic_fields.bits.weighted_pred_flag = 1;
	pic.pic_fields.bits.entropy_coding_sync_enabled_flag = 1;
	pic.pic_fields.bits.pps_loop_filter_across_slices_enabled_flag = 1;
	pic.log2_parallel_merge_level_minus2 = 0;
	pic.pps_cb_qp_offset = 0;
	pic.pps_cr_qp_offset = 0;

	n = hevc_build_vps(out, sizeof(out), &pic);
	if (n != 23) return 1;
	printf("VPS (%d): ", n);
	for (int i = 0; i < n; i++) printf("%02x", out[i]);
	printf("\n");
	n = hevc_build_sps(out, sizeof(out), &pic);
	if (n != 42) return 1;
	printf("SPS (%d): ", n);
	for (int i = 0; i < n; i++) printf("%02x", out[i]);
	printf("\n");
	n = hevc_build_pps(out, sizeof(out), &pic);
	if (n != 7) return 1;
	printf("PPS (%d): ", n);
	for (int i = 0; i < n; i++) printf("%02x", out[i]);
	printf("\n");
	return 0;
}
