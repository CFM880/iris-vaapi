// SPDX-License-Identifier: GPL-2.0-or-later
/* Firmware session lifecycle and stream boundaries.
 *
 * The stateful VPU keeps its most recent picture until the next access unit
 * arrives, and clients (Chrome) do not forward Decoder::Reset() through
 * VA-API.  This module owns opening/closing/reopening the per-context session,
 * draining completed frames, EOS flushing and detecting seek boundaries. */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "decode_internal.h"

static void
target_ring_reset(struct vpu_decode_ctx *ctx)
{
	memset(ctx->target_ring, 0, sizeof(ctx->target_ring));
}

/* Clear only state owned by one V4L2 firmware session.  Keep the picture that
 * may already have been collected between vaBeginPicture and vaEndPicture.
 *
 * This distinction matters after EOS: ensure_decoder() runs from
 * vaEndPicture, after the client has supplied the next picture.  Calling the
 * full reset_stream_state() there used to erase that picture's slice data and
 * parameters, so the first access unit after a seek/flush was incomplete. */
static void
reset_decoder_session_state(struct vpu_decode_ctx *ctx)
{
	ctx->dec_started = 0;
	ctx->eos_sent = 0;
	ctx->last_target = 0;
	ctx->hevc_ring_head = 0;
	ctx->hevc_ring_len = 0;
	ctx->direct_error = 0;
	ctx->fatal_error = 0;
	ctx->vk_capture_generation = 0;
	memset(ctx->vk_capture_keys, 0, sizeof(ctx->vk_capture_keys));
	/* A new firmware session gets a fresh GPU-copy attempt even if the
	 * previous session hit a transient Vulkan import/submit failure. */
	ctx->vk_copy_failed = 0;
	target_ring_reset(ctx);
	ctx->seq = 0;
	ctx->field_open = 0;
	ctx->second_field = 0;
	ctx->field_ts = 0;
	ctx->last_submit_ns = 0;
	ctx->vp9_seek_barrier = 0;
	vpu_codec_reset_session(ctx->codec_adapter);
}

/* Clear all per-stream decode state: parameter-set caches, the sequence to
 * surface mapping and EOS bookkeeping.  Surfaces and their backings are
 * preserved so clients may keep exporting them. */
void
reset_stream_state(struct vpu_decode_ctx *ctx)
{
	reset_decoder_session_state(ctx);
	if (ctx->codec_adapter)
		vpu_codec_begin_picture(ctx->codec_adapter);
}

unsigned int
direct_collect_surfaces(struct vpu_decode_ctx *ctx)
{
	unsigned int i, count = 0;

	for (i = 0; i < (unsigned int)ctx->surfs->n &&
	     count < ARRAY_SIZE(ctx->direct); i++) {
		struct vpu_surface *s = &ctx->surfs->s[i];

		if (s->sw != ctx->width || s->sh != ctx->height ||
		    s->fourcc != ctx->pixel_format || s->bfd < 0 ||
		    (s->owner && s->owner != ctx))
			continue;
		ctx->direct[count].id = s->id;
		ctx->direct[count].fd = s->bfd;
		ctx->direct[count].size = s->bsize;
		count++;
	}
	ctx->direct_count = count;
	return count;
}

int
direct_surface_index(struct vpu_decode_ctx *ctx, VASurfaceID id)
{
	unsigned int i;

	for (i = 0; i < ctx->direct_count; i++)
		if (ctx->direct[i].id == id)
			return (int)i;
	return -1;
}

int
ensure_decoder(struct vpu_decode_ctx *ctx)
{
	int ret;

	if (ctx->dec_open) {
		/* After an EOS flush the firmware is done; a client that keeps
		 * decoding (Chrome flush/reset, looped playback) needs a fresh
		 * session.  Preserve the picture already collected for this
		 * vaEndPicture call while clearing the old firmware bookkeeping. */
		if (ctx->eos_sent) {
			finish_pending_writes(ctx);
			forget_vk_capture_buffers(ctx);
			vpu_platform_session_close(ctx->session);
			ctx->dec_open = 0;
			reset_decoder_session_state(ctx);
		} else {
			return 0;
		}
	}
	ret = vpu_platform_session_open(ctx->session, ctx->width, ctx->height,
			       ctx->codec, ctx->pixel_format);
	if (ret)
		return ret;
	if (ctx->direct_capture) {
		int fds[VPU_MAX_SURFACES];
		size_t sizes[VPU_MAX_SURFACES];
		unsigned int i;

		direct_collect_surfaces(ctx);
		if (ctx->direct_count < ctx->direct_requested_count) {
			vpu_platform_session_close(ctx->session);
			return -EINVAL;
		}
		ctx->direct_count = ctx->direct_requested_count;
		for (i = 0; i < ctx->direct_count; i++) {
			fds[i] = ctx->direct[i].fd;
			sizes[i] = ctx->direct[i].size;
		}
		ret = vpu_platform_session_set_capture_dmabufs(ctx->session, fds, sizes,
						     ctx->direct_count);
		if (ret) {
			vpu_platform_session_close(ctx->session);
			return ret;
		}
	}
	ctx->dec_open = 1;
	return 0;
}

/* Drop a live firmware session at a random-access boundary without touching
 * the picture currently being assembled.  Chromium's decoder Reset() does
 * not issue any VA-API operation, so a seek otherwise leaves old DPB and
 * CAPTURE work in the stateful V4L2 session until the first post-seek frame
 * arrives. */
static void
restart_decoder_session(struct vpu_decode_ctx *ctx)
{
	int i;

	if (!ctx->dec_open)
		return;
	finish_pending_writes(ctx);
	forget_vk_capture_buffers(ctx);
	vpu_platform_session_close(ctx->session);
	ctx->dec_open = 0;
	/* Pending pictures from the abandoned stream keep their stable backing,
	 * but no longer have firmware work that a later vaSyncSurface can drain. */
	if (ctx->surfs) {
		for (i = 0; i < ctx->surfs->n; i++) {
			struct vpu_surface *s = &ctx->surfs->s[i];

			if (s->owner == ctx && s->queued && !s->decoded)
				s->queued = 0;
		}
	}
	reset_decoder_session_state(ctx);
}

int
drain_available(struct vpu_decode_ctx *ctx)
{
	int copy_ret;

	if (ctx->fatal_error)
		return ctx->fatal_error;
	copy_ret = reap_vk_copies(ctx, 0);
	if (copy_ret < 0)
		return copy_ret;
	if (ctx->eos_sent)
		return 0;

	for (;;) {
		struct vpu_decoded_frame frame;
		int changed, ret;

		ret = vpu_platform_session_poll(ctx->session, 0);
		if (ret < 0) {
			ctx->fatal_error = ret;
			return ret;
		}
		if (!ret)
			break;
		ret = vpu_platform_session_handle_events(ctx->session, &changed);
		if (ret)
			return ret;
		while (vpu_platform_session_dequeue_input(ctx->session) == 0)
			;
		ret = vpu_platform_session_dequeue_frame(ctx->session, &frame);
		if (ret == -EAGAIN)
			break;
		if (ret < 0) {
			ctx->fatal_error = ret;
			return ret;
		}
		ret = assign_frame(ctx, &frame);
		if (ret < 0) {
			ctx->fatal_error = ret;
			return ret;
		}
		if (ctx->direct_error)
			return ctx->direct_error;
		ret = reap_vk_copies(ctx, 0);
		if (ret < 0)
			return ret;
	}
	return 0;
}

/* Wait for one already-submitted render target without flushing the stream.
 * The V4L2 session requests decode-order output, so firmware can complete the
 * current target without needing another picture to be queued.
 * This is used before vaEndPicture returns: the ANGLE/GL import path on legacy
 * Adreno does not reliably wait for reservation fences attached after the
 * DMA-BUF was imported, and can otherwise sample that surface's old pixels. */
int
wait_surface_ready(struct vpu_decode_ctx *ctx, VASurfaceID id, int deadline)
{
	while (deadline-- > 0) {
		struct vpu_surface *s;
		struct vpu_decoded_frame frame;
		int changed, ret;

		ret = drain_available(ctx);
		if (ret)
			return ret;
		ret = finish_vk_surface(ctx, id, 1);
		if (ret && ret != -EAGAIN)
			return ret;
		s = find_surface(ctx, id);
		if (s && s->decoded)
			return 0;

		ret = vpu_platform_session_poll_capture(ctx->session, 20);
		if (ret < 0)
			return ret;
		if (!ret)
			continue;
		ret = vpu_platform_session_handle_events(ctx->session, &changed);
		if (ret)
			return ret;
		while (vpu_platform_session_dequeue_input(ctx->session) == 0)
			;
		ret = vpu_platform_session_dequeue_frame(ctx->session, &frame);
		if (ret == -EAGAIN)
			continue;
		if (ret < 0) {
			ctx->fatal_error = ret;
			return ret;
		}
		ret = assign_frame(ctx, &frame);
		if (ret < 0) {
			ctx->fatal_error = ret;
			return ret;
		}
	}
	return -ETIMEDOUT;
}

/* Force the firmware to release the picture it is holding.  The stateful
 * decoder keeps the most recent picture until the next access unit (or an
 * EOS marker) arrives; without this, vaSyncSurface on the last picture
 * always times out. */
static int
vpu_decode_flush_impl(struct vpu_decode_ctx *ctx)
{
	struct vpu_decoded_frame frame;
	int ret, deadline = 100;

	if (!ctx->dec_open || !ctx->dec_started || ctx->eos_sent)
		return ctx->fatal_error;

	ret = vpu_platform_session_flush(ctx->session);
	if (ret)
		return ret;
	ctx->eos_sent = 1;

	/* Drain decoded pictures until the empty V4L2_BUF_FLAG_LAST marker is
	 * dequeued.  Pictures preceding that marker are assigned normally. */
	while (deadline-- > 0) {
		int changed;

		ret = vpu_platform_session_poll_capture(ctx->session, 20);
		if (ret < 0) {
			ctx->fatal_error = ret;
			return ret;
		}
		if (!ret)
			continue;
		ret = vpu_platform_session_handle_events(ctx->session, &changed);
		if (ret)
			return ret;
		while (vpu_platform_session_dequeue_input(ctx->session) == 0)
			;
		ret = vpu_platform_session_dequeue_frame(ctx->session, &frame);
		if (ret == -EAGAIN)
			continue;
		if (ret < 0) {
			ctx->fatal_error = ret;
			return ret;
		}
		DBG("[flush] got ts=%llu flags=0x%x\n",
		    (unsigned long long)frame.timestamp, frame.flags);
		if (frame.bytesused && assign_frame(ctx, &frame) < 0) {
			ctx->fatal_error = -EIO;
			return ctx->fatal_error;
		}
		if (ret == 1)
			break;
	}
	if (vpu_platform_session_eos(ctx->session)) {
		ret = reap_vk_copies(ctx, 1);
		return ret;
	}
	return -ETIMEDOUT;
}

/* Establish a strict stream boundary.  Chromium does not forward
 * Decoder::Reset() through VA-API, so the first post-seek key frame is the
 * earliest point where the driver can act.  Finish the complete old
 * OUTPUT/CAPTURE pipeline and observe LAST before closing it; only then may the
 * already assembled key frame be submitted to a fresh firmware session.
 * Preserve the private sequence epoch across every codec restart; legacy VPU5
 * VP9 has proved sensitive to a timestamp rewind across a context reused by
 * Chromium. */
int
stream_boundary_restart(struct vpu_decode_ctx *ctx)
{
	uint64_t next_seq = ctx->seq;
	unsigned int pending = 0, i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(ctx->target_ring); i++)
		pending += ctx->target_ring[i].used != 0;
	if (ctx->codec == VPU_CODEC_HEVC &&
	    (ctx->platform_quirks & VPU_PLATFORM_QUIRK_HEVC_CAPTURE_FIFO))
		pending = ctx->hevc_ring_len;
	DBG("[seek] draining %u pre-seek mappings codec=0x%x\n",
	    pending, ctx->codec);
	ret = vpu_decode_flush_impl(ctx);
	if (ret) {
		DBG("[seek] pre-seek drain failed codec=0x%x: %d\n",
		    ctx->codec, ret);
		return ret;
	}
	restart_decoder_session(ctx);
	ctx->seq = next_seq;
	/* VP9 presents already-decoded surfaces through show_existing_frame after
	 * a seek, so it keeps the display epoch.  H.264/HEVC abandon the old
	 * backings: any surface not re-decoded in the new epoch is stale. */
	if (ctx->codec != VPU_CODEC_VP9)
		surfs_begin_epoch(ctx->surfs);
	ctx->vp9_seek_barrier = ctx->codec == VPU_CODEC_VP9 &&
		(ctx->platform_quirks & VPU_PLATFORM_QUIRK_VP9_RELEASE_AU);
	DBG("[seek] old session complete codec=0x%x; restart at seq=%llu\n",
	    ctx->codec, (unsigned long long)ctx->seq);
	return 0;
}

int
vpu_decode_flush(struct vpu_decode_ctx *ctx)
{
	int ret;

	pthread_mutex_lock(&ctx->mutex);
	ret = vpu_decode_flush_impl(ctx);
	pthread_mutex_unlock(&ctx->mutex);
	return ret;
}

/* Tear down the firmware session and all stream state (Chrome Flush/Reset,
 * seeks).  Surfaces and their backings are preserved so frames already
 * exported to the client stay valid. */
void
vpu_decode_reset(struct vpu_decode_ctx *ctx)
{
	if (!ctx)
		return;
	pthread_mutex_lock(&ctx->mutex);
	if (ctx->dec_open) {
		finish_pending_writes(ctx);
		forget_vk_capture_buffers(ctx);
		vpu_platform_session_close(ctx->session);
		ctx->dec_open = 0;
	}
	reset_stream_state(ctx);
	pthread_mutex_unlock(&ctx->mutex);
}
