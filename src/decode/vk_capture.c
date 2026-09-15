// SPDX-License-Identifier: GPL-2.0-or-later
/* Lifecycle of the opt-in Vulkan DMA-BUF capture copies.
 *
 * A decoded frame is copied GPU-side into the surface's stable backing so the
 * exported buffer stays valid.  Copies are asynchronous: the pending ring
 * tracks each in-flight job until it completes, the surface is recycled and
 * its reservation fence signalled. */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "decode_internal.h"

static int
finish_vk_copy(struct vpu_decode_ctx *ctx, unsigned int index, int wait)
{
	typeof(ctx->pending_copies[0]) *pending = &ctx->pending_copies[index];
	struct vpu_surface *s;
	int ret, finish_ret = 0;

	if (!pending->used)
		return 0;
	ret = vpu_vk_copy_job_wait(ctx->vk_copy, pending->job,
				    wait ? UINT64_MAX : 0);
	if (ret <= 0)
		return ret;

	s = find_surface(ctx, pending->id);
	if (s && s->generation == pending->generation &&
	    s->fence_token == pending->fence_token) {
		finish_ret = surface_finish_write(ctx->session, s);
		s->initialized = 1;
		surfaces_mark_decoded(ctx, s);
	} else if (pending->fence_token) {
		/* The VA client should not destroy/recycle an in-flight target,
		 * but never leave its reservation fence permanently unsignalled if
		 * it does. */
		finish_ret = vpu_platform_session_signal_surface_fence(
			ctx->session, pending->fence_token);
	}
	ret = vpu_platform_session_requeue_index(ctx->session, pending->capture_index);
	if (!finish_ret && ret)
		finish_ret = ret;
	if (ctx->stats_enabled) {
		ctx->stats_vk_copy_ns += vpu_monotonic_ns() - pending->start_ns;
		ctx->stats_vk_copy_bytes += pending->bytes;
		ctx->stats_vk_copy_frames++;
	}
	vpu_vk_copy_job_release(pending->job);
	memset(pending, 0, sizeof(*pending));
	return finish_ret ? finish_ret : 1;
}

int
reap_vk_copies(struct vpu_decode_ctx *ctx, int wait_all)
{
	unsigned int i;
	int ret;

	if (!ctx->vk_copy)
		return 0;
	for (i = 0; i < ARRAY_SIZE(ctx->pending_copies); i++) {
		if (!ctx->pending_copies[i].used)
			continue;
		ret = finish_vk_copy(ctx, i, wait_all);
		if (ret < 0)
			return ret;
	}
	return 0;
}

void
forget_vk_capture_buffers(struct vpu_decode_ctx *ctx)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ctx->vk_capture_keys); i++) {
		if (ctx->vk_capture_keys[i])
			vpu_vk_copy_forget(ctx->vk_copy,
					    ctx->vk_capture_keys[i]);
	}
	ctx->vk_capture_generation = 0;
	memset(ctx->vk_capture_keys, 0, sizeof(ctx->vk_capture_keys));
}

int
finish_vk_surface(struct vpu_decode_ctx *ctx, VASurfaceID id, int wait)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ctx->pending_copies); i++) {
		int ret;

		if (!ctx->pending_copies[i].used ||
		    ctx->pending_copies[i].id != id)
			continue;
		ret = finish_vk_copy(ctx, i, wait);
		if (ret < 0)
			return ret;
		if (!ret)
			return -EAGAIN;
	}
	return 0;
}

int
surface_vk_submit(struct vpu_decode_ctx *ctx, struct vpu_surface *s,
		  const struct vpu_decoded_frame *frame)
{
	unsigned int pitch, source_size;
	unsigned int slot;
	struct vpu_vk_job *job;
	int source_fd, ret, completed = 0;

	if (!ctx->vk_copy || ctx->vk_copy_failed)
		return -ENOTSUP;
	if (frame->index >= ARRAY_SIZE(ctx->vk_capture_keys))
		return -ERANGE;
	if (ctx->vk_capture_generation != frame->capture_generation) {
		ret = reap_vk_copies(ctx, 1);
		if (ret)
			return ret;
		forget_vk_capture_buffers(ctx);
		ctx->vk_capture_generation = frame->capture_generation;
	}
	if (!ctx->vk_capture_keys[frame->index])
		ctx->vk_capture_keys[frame->index] = __atomic_add_fetch(
			&g_buffer_serial, 1, __ATOMIC_RELAXED);
	ret = vpu_platform_session_export_frame(ctx->session, frame->index, &source_fd, &pitch,
			      &source_size);
	if (ret)
		return ret;
	/* Chrome imports stable surfaces before decode.  Legacy ANGLE does not
	 * reliably observe reservation fences attached after that import, so
	 * finish the GPU copy before returning a dequeued exported surface.  The
	 * decode-only path below remains fully asynchronous. */
	if (s->exported) {
		uint64_t start = ctx->stats_enabled ? vpu_monotonic_ns() : 0;

		ret = vpu_vk_copy_dmabuf(ctx->vk_copy,
					  ctx->vk_capture_keys[frame->index],
					  source_fd, source_size,
					  s->backing_serial, s->bfd, s->bsize,
					  frame->bytesused);
		if (!ret)
			ret = surface_finish_write(ctx->session, s);
		if (!ret) {
			if (ctx->stats_enabled) {
				ctx->stats_vk_copy_ns += vpu_monotonic_ns() - start;
				ctx->stats_vk_copy_bytes += frame->bytesused;
				ctx->stats_vk_copy_frames++;
			}
			completed = 1;
		}
		goto out;
	}
	for (slot = 0; slot < ARRAY_SIZE(ctx->pending_copies); slot++)
		if (!ctx->pending_copies[slot].used)
			break;
	if (slot == ARRAY_SIZE(ctx->pending_copies)) {
		ret = reap_vk_copies(ctx, 0);
		if (ret)
			goto out;
		for (slot = 0; slot < ARRAY_SIZE(ctx->pending_copies); slot++)
			if (!ctx->pending_copies[slot].used)
				break;
	}
	if (slot == ARRAY_SIZE(ctx->pending_copies)) {
		/* V4L2 currently has at most 20 CAPTURE buffers, so this is only
		 * a defensive pressure valve. */
		ret = finish_vk_copy(ctx, 0, 1);
		if (ret < 0)
			goto out;
		slot = 0;
	}

	ret = vpu_vk_copy_submit(ctx->vk_copy,
				 ctx->vk_capture_keys[frame->index],
				 source_fd, source_size,
				 s->backing_serial, s->bfd, s->bsize,
				 frame->bytesused, &job);
	if (!ret) {
		ctx->pending_copies[slot].job = job;
		ctx->pending_copies[slot].id = s->id;
		ctx->pending_copies[slot].generation = s->generation;
		ctx->pending_copies[slot].fence_token = s->fence_token;
		ctx->pending_copies[slot].start_ns = vpu_monotonic_ns();
		ctx->pending_copies[slot].bytes = frame->bytesused;
		ctx->pending_copies[slot].capture_index = frame->index;
		ctx->pending_copies[slot].used = 1;
	}
out:
	close(source_fd);
	if (ret) {
		ctx->stats_vk_copy_fallbacks++;
		if (!ctx->vk_copy_failed)
			fprintf(stderr,
				"vpu-vaapi: Vulkan DMA-BUF copy failed (%s); using CPU copy\n",
				strerror(-ret));
		ctx->vk_copy_failed = 1;
		return ret;
	}
	return completed ? 1 : 0;
}

void
finish_pending_writes(struct vpu_decode_ctx *ctx)
{
	int i;

	if (!ctx->surfs || !ctx->dec_open)
		return;
	(void)reap_vk_copies(ctx, 1);
	for (i = 0; i < ctx->surfs->n; i++) {
		struct vpu_surface *s = &ctx->surfs->s[i];

		if (s->owner == ctx && (s->write_started || s->fence_token))
			surface_finish_write(ctx->session, s);
	}
}
