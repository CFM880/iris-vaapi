// SPDX-License-Identifier: GPL-2.0-or-later

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "decode.h"
#include "h264_params.h"
#include "v4l2_dec.h"

#define IRIS_MAX_SURFACES	64
#define IRIS_AU_MAX		(16U * 1024 * 1024)
#define NO_CAP			(~0U)

struct iris_surface {
	VASurfaceID id;
	unsigned int cap_index;	/* engine CAPTURE buffer holding the frame */
	int decoded;
};

struct iris_decode_ctx {
	struct v4l2_dec dec;
	int dec_open;
	int dec_started;
	unsigned int width, height;
	VAProfile profile;

	struct iris_surface surfaces[IRIS_MAX_SURFACES];
	int n_surfaces;

	VAPictureParameterBufferH264 pic;
	int have_pic;
	uint8_t *slice_data;
	size_t slice_len;
	size_t slice_cap;
	VASurfaceID current_target;

	uint8_t last_sps[256];
	int last_sps_len;
	uint8_t last_pps[128];
	int last_pps_len;
	int refs_l0, refs_l1;	/* from slice params, for the PPS default */

	/* Map decode sequence numbers back to target surfaces so frame
	 * matching does not depend on the (possibly non-contiguous) VASurfaceID
	 * values that the client happens to use. */
	uint64_t seq;
	VASurfaceID target_of[512];
};

static struct iris_surface *
find_surface(struct iris_decode_ctx *ctx, VASurfaceID id)
{
	int i;

	for (i = 0; i < ctx->n_surfaces; i++)
		if (ctx->surfaces[i].id == id)
			return &ctx->surfaces[i];
	return NULL;
}

struct iris_decode_ctx *
iris_decode_create(void)
{
	return calloc(1, sizeof(struct iris_decode_ctx));
}

void
iris_decode_destroy(struct iris_decode_ctx *ctx)
{
	if (!ctx)
		return;
	if (ctx->dec_open)
		v4l2_dec_close(&ctx->dec);
	free(ctx->slice_data);
	free(ctx);
}

void
iris_decode_setup(struct iris_decode_ctx *ctx, unsigned int width,
		  unsigned int height, VAProfile profile)
{
	ctx->width = width;
	ctx->height = height;
	ctx->profile = profile;
}

int
iris_decode_create_surface(struct iris_decode_ctx *ctx, VASurfaceID id)
{
	struct iris_surface *s;

	if (ctx->n_surfaces >= IRIS_MAX_SURFACES)
		return -1;
	s = &ctx->surfaces[ctx->n_surfaces++];
	s->id = id;
	s->cap_index = NO_CAP;
	s->decoded = 0;
	return 0;
}

int
iris_decode_destroy_surface(struct iris_decode_ctx *ctx, VASurfaceID id)
{
	int i;

	for (i = 0; i < ctx->n_surfaces; i++) {
		if (ctx->surfaces[i].id != id)
			continue;
		if (ctx->dec_open && ctx->surfaces[i].cap_index != NO_CAP)
			v4l2_dec_qcap_idx(&ctx->dec,
					  ctx->surfaces[i].cap_index);
		ctx->surfaces[i] = ctx->surfaces[ctx->n_surfaces - 1];
		ctx->n_surfaces--;
		return 0;
	}
	return -1;
}

static int
ensure_decoder(struct iris_decode_ctx *ctx)
{
	int ret;

	if (ctx->dec_open)
		return 0;
	ret = v4l2_dec_open(&ctx->dec, "/dev/video0", ctx->width, ctx->height);
	if (ret)
		return ret;
	ctx->dec_open = 1;
	return 0;
}

/* Assign one dequeued frame to its surface (matched by the timestamp we
 * encoded as the surface id).  Returns the surface id, or -1 if unknown. */
static int
assign_frame(struct iris_decode_ctx *ctx, const struct v4l2_dec_frame *frame)
{
	uint64_t seq = (frame->timestamp / 1000000000ULL) - 1000;
	VASurfaceID id = (seq < 512) ? ctx->target_of[seq] : 0;
	struct iris_surface *s = find_surface(ctx, id);

	if (!s) {
		v4l2_dec_qcap_idx(&ctx->dec, frame->index);
		return -1;
	}
	if (s->cap_index != NO_CAP)
		v4l2_dec_qcap_idx(&ctx->dec, s->cap_index);
	s->cap_index = frame->index;
	s->decoded = 1;
	return id;
}

/* Non-blocking drain of whatever frames are ready. */
static int
drain_available(struct iris_decode_ctx *ctx)
{
	for (;;) {
		struct v4l2_dec_frame frame;
		int changed, ret;

		ret = v4l2_dec_poll(&ctx->dec, 0);
		if (ret <= 0)
			break;
		ret = v4l2_dec_handle_events(&ctx->dec, &changed);
		if (ret)
			return ret;
		while (v4l2_dec_dqout(&ctx->dec) == 0)
			;
		ret = v4l2_dec_dqcap(&ctx->dec, &frame);
		if (ret < 0)
			break;
		assign_frame(ctx, &frame);
	}
	return 0;
}

int
iris_decode_begin(struct iris_decode_ctx *ctx, VASurfaceID target)
{
	ctx->current_target = target;
	ctx->have_pic = 0;
	ctx->slice_len = 0;
	ctx->refs_l0 = 0;
	ctx->refs_l1 = 0;
	return 0;
}

int
iris_decode_slice_params(struct iris_decode_ctx *ctx,
			 const VASliceParameterBufferH264 *sp)
{
		if (sp->num_ref_idx_l0_active_minus1 > ctx->refs_l0)
		ctx->refs_l0 = sp->num_ref_idx_l0_active_minus1;
	if (sp->num_ref_idx_l1_active_minus1 > ctx->refs_l1)
		ctx->refs_l1 = sp->num_ref_idx_l1_active_minus1;
	return 0;
}

int
iris_decode_picture(struct iris_decode_ctx *ctx,
		    const VAPictureParameterBufferH264 *pic)
{
	memcpy(&ctx->pic, pic, sizeof(*pic));
	ctx->have_pic = 1;
	return 0;
}

static int
has_start_code(const uint8_t *p, size_t len)
{
	if (len >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1)
		return 4;
	if (len >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1)
		return 3;
	return 0;
}

int
iris_decode_slice(struct iris_decode_ctx *ctx, const void *data, size_t len)
{
	const uint8_t *p = data;
	size_t need = len + 4;

	if (ctx->slice_len + need > ctx->slice_cap) {
		size_t ncap = ctx->slice_cap ? ctx->slice_cap * 2 :
					      (len + (1 << 20));
		void *n = realloc(ctx->slice_data, ncap);

		if (!n)
			return -1;
		ctx->slice_data = n;
		ctx->slice_cap = ncap;
	}

	if (!has_start_code(p, len)) {
		static const uint8_t sc4[4] = { 0, 0, 0, 1 };
		memcpy(ctx->slice_data + ctx->slice_len, sc4, 4);
		ctx->slice_len += 4;
	}
	memcpy(ctx->slice_data + ctx->slice_len, p, len);
	ctx->slice_len += len;
	return 0;
}

int
iris_decode_end(struct iris_decode_ctx *ctx)
{
	uint8_t *au;
	size_t au_len = 0, au_cap;
	int n, ret;
	static const uint8_t sc4[4] = { 0, 0, 0, 1 };

	if (!ctx->have_pic)
		return -1;

	/* 16 MiB of stack would overflow the caller's stack; use the heap. */
	au_cap = ctx->slice_len + 256;
	au = malloc(au_cap);
	if (!au)
		return -1;
	ret = ensure_decoder(ctx);
	if (ret) {
		return ret;
	}

	/* Only re-emit SPS/PPS when they change; a per-picture repetition
	 * resets the firmware DPB and breaks P-frame references. */
	n = h264_build_sps(au + 4, au_cap - 4 - ctx->slice_len, &ctx->pic,
			   ctx->profile);
	if (n <= 0) {
		return -1;
	}
	if (n != ctx->last_sps_len ||
	    memcmp(au + 4, ctx->last_sps, n) != 0) {
		memcpy(au, sc4, 4);
		memcpy(ctx->last_sps, au + 4, n);
		ctx->last_sps_len = n;
		au_len = 4 + n;
	}

		n = h264_build_pps(au + au_len + 4, au_cap - au_len - 4 -
			   ctx->slice_len, &ctx->pic, ctx->refs_l0, ctx->refs_l1);
	if (n <= 0)
		return -1;
	if (n != ctx->last_pps_len ||
	    memcmp(au + au_len + 4, ctx->last_pps, n) != 0) {
		memcpy(au + au_len, sc4, 4);
		memcpy(ctx->last_pps, au + au_len + 4, n);
		ctx->last_pps_len = n;
		au_len += 4 + n;
	}

	if (au_len + ctx->slice_len > au_cap) {
		free(au);
		return -1;
	}
	memcpy(au + au_len, ctx->slice_data, ctx->slice_len);
	au_len += ctx->slice_len;

	{
		uint64_t ts = (ctx->seq + 1000) * 1000000000ULL;

		if (ctx->seq < 512)
			ctx->target_of[ctx->seq] = ctx->current_target;
		ctx->seq++;

		if (!ctx->dec_started) {
			/* Mirror FFmpeg: queue the first access unit before
			 * STREAMON, otherwise the firmware rejects the CAPTURE
			 * setup (0x1004). */
			ret = v4l2_dec_feed(&ctx->dec, au, au_len, ts);
			if (ret) {
				free(au);
				return ret;
			}
			ret = v4l2_dec_start(&ctx->dec);
			if (ret) {
				free(au);
				return ret;
			}
			ctx->dec_started = 1;
		} else {
			/* Wait for a free OUTPUT buffer, then queue.  The firmware
			 * stops consuming input when its CAPTURE queue fills, so
			 * drain finished frames while we wait or we deadlock. */
			for (int spin = 0; spin < 100; spin++) {
				while (v4l2_dec_dqout(&ctx->dec) == 0)
					;
				ret = v4l2_dec_feed(&ctx->dec, au, au_len, ts);
				if (ret != -EAGAIN)
					break;
				drain_available(ctx);
				v4l2_dec_poll(&ctx->dec, 50);
			}
						if (ret) {
				free(au);
				return ret;
			}
		}
	}
	free(au);

	/* The stateful firmware holds each frame until the next access unit
	 * arrives.  Draining right after this feed makes the *previous*
	 * picture's frame available so a client that syncs one picture at a
	 * time (ffmpeg/Chrome) does not deadlock. */
	drain_available(ctx);

	ctx->slice_len = 0;
	ctx->have_pic = 0;
	return 0;
}

int
iris_decode_sync(struct iris_decode_ctx *ctx, VASurfaceID id)
{
	int deadline = 200;	/* ~2 s */


	if (iris_decode_surface_ready(ctx, id))
		return 0;

	while (deadline-- > 0) {
		struct v4l2_dec_frame frame;
		int changed, ret;

		ret = v4l2_dec_poll(&ctx->dec, 20);
		if (ret <= 0)
			continue;
		(void)changed;
		ret = v4l2_dec_handle_events(&ctx->dec, &changed);
		if (ret)
			return ret;
		while (v4l2_dec_dqout(&ctx->dec) == 0)
			;
		ret = v4l2_dec_dqcap(&ctx->dec, &frame);
		if (ret < 0)
			continue;
			assign_frame(ctx, &frame);
		if (iris_decode_surface_ready(ctx, id))
			return 0;
	}
	return -1;
}

int
iris_decode_surface_ready(struct iris_decode_ctx *ctx, VASurfaceID id)
{
	drain_available(ctx);
	{
		struct iris_surface *s = find_surface(ctx, id);

		return s ? s->decoded : 0;
	}
}

int
iris_decode_surface_buffer(struct iris_decode_ctx *ctx, VASurfaceID id,
			   unsigned int *cap_index, void **mem,
			   unsigned int *pitch, unsigned int *size,
			   unsigned int *width, unsigned int *height)
{
	struct iris_surface *s = find_surface(ctx, id);

	if (!s || s->cap_index == NO_CAP)
		return -1;
	*cap_index = s->cap_index;
	*mem = ctx->dec.cap_mem[s->cap_index];
	*pitch = ctx->dec.cap_fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
	*size = ctx->dec.cap_size[s->cap_index];
	*width = ctx->dec.width;
	*height = ctx->dec.height;
	return 0;
}

int
iris_decode_export(struct iris_decode_ctx *ctx, VASurfaceID id, int *fd,
		   unsigned int *pitch, unsigned int *size,
		   unsigned int *width, unsigned int *height)
{
	struct iris_surface *s = find_surface(ctx, id);

	if (!s || s->cap_index == NO_CAP)
		return -1;
	if (v4l2_dec_export(&ctx->dec, s->cap_index, fd, pitch, size))
		return -1;
	v4l2_dec_size(&ctx->dec, width, height);
	return 0;
}