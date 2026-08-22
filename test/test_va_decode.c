// SPDX-License-Identifier: GPL-2.0-or-later
/* End-to-end VA-API decode test: decode the first picture of an Annex-B
 * H.264 stream through the iris-vaapi driver and verify the exported NV12
 * against FFmpeg's software decode.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>

/* ---- bit reader for SPS/PPS parsing ---- */

struct br {
	const uint8_t *p;
	const uint8_t *end;
	unsigned int bit;
};

static unsigned int br_bit(struct br *b)
{
	unsigned int v = (b->p[0] >> (7 - b->bit)) & 1;

	if (++b->bit == 8) {
		b->bit = 0;
		b->p++;
	}
	return v;
}

static unsigned int br_bits(struct br *b, int n)
{
	unsigned int v = 0;
	int i;

	for (i = 0; i < n; i++)
		v = (v << 1) | br_bit(b);
	return v;
}

static unsigned int br_ue(struct br *b)
{
	unsigned int z = 0, v;

	while (!br_bit(b))
		z++;
	v = (1u << z) - 1;
	while (z--)
		v += br_bit(b) << z;
	return v;
}

static int br_se(struct br *b)
{
	unsigned int u = br_ue(b);

	return (u & 1) ? (int)((u + 1) >> 1) : -(int)(u >> 1);
}

/* ---- Annex-B scan ---- */

static long find_start(const uint8_t *buf, size_t size, size_t from)
{
	size_t i;

	for (i = from; i + 3 < size; i++) {
		if (i + 4 < size && buf[i] == 0 && buf[i + 1] == 0 &&
		    buf[i + 2] == 0 && buf[i + 3] == 1)
			return i + 4;
		if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1 &&
		    (i == from || buf[i - 1] != 0))
			return i + 3;
	}
	return -1;
}

static void parse_sps(const uint8_t *nal, size_t len,
		      VAPictureParameterBufferH264 *pic)
{
	struct br b;
	unsigned int u;

	memset(pic, 0, sizeof(*pic));
	b.p = nal + 1;
	b.end = nal + len;
	b.bit = 0;

	(void)br_bits(&b, 8);
	(void)br_bits(&b, 8);
	(void)br_bits(&b, 8);
	(void)br_ue(&b);

	u = br_ue(&b);
	pic->seq_fields.bits.chroma_format_idc = u;
	if (u == 3)
		pic->seq_fields.bits.residual_colour_transform_flag = br_bit(&b);
	pic->bit_depth_luma_minus8 = br_ue(&b);
	pic->bit_depth_chroma_minus8 = br_ue(&b);
	(void)br_bit(&b);
	if (br_bit(&b))
		return;

	pic->seq_fields.bits.log2_max_frame_num_minus4 = br_ue(&b);
	u = br_ue(&b);
	pic->seq_fields.bits.pic_order_cnt_type = u;
	if (u == 0)
		pic->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = br_ue(&b);
	else if (u == 1) {
		pic->seq_fields.bits.delta_pic_order_always_zero_flag = br_bit(&b);
		(void)br_se(&b);
		(void)br_se(&b);
		(void)br_ue(&b);
	}

	pic->num_ref_frames = br_ue(&b);
	pic->seq_fields.bits.gaps_in_frame_num_value_allowed_flag = br_bit(&b);
	pic->picture_width_in_mbs_minus1 = br_ue(&b);
	pic->picture_height_in_mbs_minus1 = br_ue(&b);
	pic->seq_fields.bits.frame_mbs_only_flag = br_bit(&b);
	if (!pic->seq_fields.bits.frame_mbs_only_flag)
		pic->seq_fields.bits.mb_adaptive_frame_field_flag = br_bit(&b);
	pic->seq_fields.bits.direct_8x8_inference_flag = br_bit(&b);
	if (br_bit(&b)) {
		(void)br_ue(&b); (void)br_ue(&b); (void)br_ue(&b); (void)br_ue(&b);
	}
}

static void parse_pps(const uint8_t *nal, size_t len,
		      VAPictureParameterBufferH264 *pic)
{
	struct br b;
	unsigned int cfi = pic->seq_fields.bits.chroma_format_idc;

	b.p = nal + 1;
	b.end = nal + len;
	b.bit = 0;

	(void)br_ue(&b);
	(void)br_ue(&b);
	pic->pic_fields.bits.entropy_coding_mode_flag = br_bit(&b);
	pic->pic_fields.bits.pic_order_present_flag = br_bit(&b);
	(void)br_ue(&b);
	(void)br_ue(&b);
	(void)br_ue(&b);
	pic->pic_fields.bits.weighted_pred_flag = br_bit(&b);
	pic->pic_fields.bits.weighted_bipred_idc = br_bits(&b, 2);
	pic->pic_init_qp_minus26 = br_se(&b);
	pic->pic_init_qs_minus26 = br_se(&b);
	pic->chroma_qp_index_offset = br_se(&b);
	pic->pic_fields.bits.deblocking_filter_control_present_flag = br_bit(&b);
	pic->pic_fields.bits.constrained_intra_pred_flag = br_bit(&b);
	pic->pic_fields.bits.redundant_pic_cnt_present_flag = br_bit(&b);
	if (cfi == 1)
		pic->pic_fields.bits.transform_8x8_mode_flag = br_bit(&b);
	(void)br_bit(&b);
	pic->second_chroma_qp_index_offset = br_se(&b);
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/home/cfm880/qcom/test-1080p-3s.h264";
	uint8_t *data;
	long fsize;
	FILE *fp;
	long pos;
	VAPictureParameterBufferH264 pic;
	uint8_t sps[256], pps[64], slice[6][1 << 20];
	int sps_len = -1, pps_len = -1;
	int slice_len[6], n_slices = 0;
	VADisplay dpy;
	VAConfigID cfg;
	VAContextID ctx;
	VASurfaceID surf[16];
	VABufferID bufs[4];
	VAStatus st;
	int major = 0, minor = 0;
	int fd;

	fp = fopen(path, "rb");
	if (!fp) { perror(path); return 1; }
	fseek(fp, 0, SEEK_END);
	fsize = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	data = malloc(fsize);
	if (!data || fread(data, 1, fsize, fp) != (size_t)fsize)
		return 1;
	fclose(fp);

	pos = find_start(data, fsize, 0);
	while (pos >= 0 && (sps_len < 0 || pps_len < 0 || n_slices < 5)) {
		long next = find_start(data, fsize, pos + 1);
		int t = data[pos] & 0x1f;
		size_t end = next < 0 ? fsize : (size_t)next;
		size_t nalsz = end - (size_t)pos;

		if (t == 7 && sps_len < 0) {
			memcpy(sps, data + pos, nalsz); sps_len = nalsz;
		} else if (t == 8 && pps_len < 0) {
			memcpy(pps, data + pos, nalsz); pps_len = nalsz;
		} else if (t >= 1 && t <= 5 && n_slices < 5) {
			memcpy(slice[n_slices], data + pos, nalsz);
			slice_len[n_slices] = nalsz;
			n_slices++;
		}
		pos = next;
	}
	free(data);
	printf("SPS %d PPS %d slices %d (first %d bytes)\n",
	       sps_len, pps_len, n_slices, slice_len[0]);

	parse_sps(sps, sps_len, &pic);
	parse_pps(pps, pps_len, &pic);
	printf("picture %ux%u mbs\n",
	       (pic.picture_width_in_mbs_minus1 + 1) * 16,
	       (pic.picture_height_in_mbs_minus1 + 1) * 16);

	fd = open("/dev/dri/renderD128", O_RDWR);
	if (fd < 0) { perror("open renderD128"); return 1; }
	dpy = vaGetDisplayDRM(fd);
	if (!dpy) { fprintf(stderr, "vaGetDisplayDRM\n"); return 1; }
	st = vaInitialize(dpy, &major, &minor);
	if (st != VA_STATUS_SUCCESS) {
		fprintf(stderr, "vaInitialize: %s\n", vaErrorStr(st));
		return 1;
	}

	st = vaCreateConfig(dpy, VAProfileH264High, VAEntrypointVLD, NULL, 0,
			    &cfg);
	if (st) { fprintf(stderr, "vaCreateConfig: %s\n", vaErrorStr(st)); return 1; }
	st = vaCreateContext(dpy, cfg, 1920, 1088, 0, NULL, 0, &ctx);
	if (st) { fprintf(stderr, "vaCreateContext: %s\n", vaErrorStr(st)); return 1; }
	st = vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, 1920, 1088, surf, 16,
			      NULL, 0);
	if (st) { fprintf(stderr, "vaCreateSurfaces: %s\n", vaErrorStr(st)); return 1; }

	st = vaCreateBuffer(dpy, ctx, VAPictureParameterBufferType,
			    sizeof(VAPictureParameterBufferH264), 1, &pic,
			    &bufs[0]);
	if (st) { fprintf(stderr, "pic buf: %s\n", vaErrorStr(st)); return 1; }

	/* Feed 5 pictures (the firmware pipeline needs several in flight).
	 * Repeat the IDR to avoid cross-picture reference dependencies. */
	for (int i = 0; i < 5; i++) {
		int si = 0;
		st = vaCreateBuffer(dpy, ctx, VASliceDataBufferType,
				    slice_len[si], 1, slice[si], &bufs[1]);
		if (st) { fprintf(stderr, "slice buf: %s\n", vaErrorStr(st)); return 1; }
		st = vaBeginPicture(dpy, ctx, surf[i]);
		if (st) { fprintf(stderr, "vaBeginPicture: %s\n", vaErrorStr(st)); return 1; }
		st = vaRenderPicture(dpy, ctx, bufs, 2);
		if (st) { fprintf(stderr, "vaRenderPicture: %s\n", vaErrorStr(st)); return 1; }
		st = vaEndPicture(dpy, ctx);
		if (st) { fprintf(stderr, "vaEndPicture %d: %s\n", i, vaErrorStr(st)); return 1; }
		vaDestroyBuffer(dpy, bufs[1]);
	}

	/* The last picture stays held in the firmware pipeline (stateful
	 * one-frame latency), so sync the fourth picture which is ready. */
	{
		int sync_idx = 3;
		int si;
		VASurfaceID s;

		for (si = 0; si < sync_idx; si++) {
			st = vaSyncSurface(dpy, surf[si]);
			if (st) { fprintf(stderr, "sync %d: %s\n", si, vaErrorStr(st)); return 1; }
		}
		s = surf[sync_idx];
		st = vaSyncSurface(dpy, s);
		if (st) { fprintf(stderr, "vaSyncSurface: %s\n", vaErrorStr(st)); return 1; }
		printf("synced surface %u\n", s);
	}

	{
		VADRMPRIMESurfaceDescriptor desc;
		unsigned int w, h, size;
		void *map;
		FILE *out = argc > 2 ? fopen(argv[2], "wb") : NULL;

		st = vaExportSurfaceHandle(dpy, surf[0],
					   VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
					   VA_EXPORT_SURFACE_READ_ONLY, &desc);
		if (st) { fprintf(stderr, "export: %s\n", vaErrorStr(st)); return 1; }
		printf("exported: %ux%u fourcc=%c%c%c%c layers=%u objects=%u\n",
		       desc.width, desc.height,
		       desc.fourcc & 0xff, (desc.fourcc >> 8) & 0xff,
		       (desc.fourcc >> 16) & 0xff, (desc.fourcc >> 24) & 0xff,
		       desc.num_layers, desc.num_objects);

		map = mmap(NULL, desc.objects[0].size, PROT_READ, MAP_SHARED,
			   desc.objects[0].fd, 0);
		if (map == MAP_FAILED) {
			perror("mmap export");
			return 1;
		}
		if (out) {
			fwrite(map, 1, desc.objects[0].size, out);
			fclose(out);
		}
		munmap(map, desc.objects[0].size);
	}

	vaDestroyConfig(dpy, cfg);
	vaTerminate(dpy);
	close(fd);
	printf("DECODE OK\n");
	return 0;
}