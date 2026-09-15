// SPDX-License-Identifier: GPL-2.0-or-later
/* Per-context access-unit pipeline: reassembles complete AUs from
 * vaRenderPicture buffers, feeds the platform session and maps decoded frames
 * back to VA surfaces.  Session lifecycle, surface registry and the Vulkan
 * copy ring live in stream.c, surface.c and vk_capture.c respectively.
 *
 * Threading/lifecycle model (matches how Chrome uses libva):
 * - Surfaces live in a display-level registry (vpu_surfaces) and may outlive
 *   the decode context that produced them.
 * - Each VA context owns its own vpu_decode_ctx (its own VPU session), so
 *   concurrent videos do not share engine state. A resized VP9 context may
 *   inherit a retired session only through explicit reference surfaces.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "decode_internal.h"

/* VA-API has no decoder-reset callback (modern libva has no vaFlush), and
 * Chromium's Reset() neither sends EOS nor recreates the VAContext.  A
 * random-access picture is therefore only treated as a stream boundary when
 * another signal proves the discontinuity:
 *   - the private session already saw EOS (client synced the last target), or
 *   - the codec adapter reports a new coded-video sequence (parameter sets
 *     changed), or
 *   - the client was idle long enough that a user seek is the likely cause.
 * Every boundary drains and reopens the firmware session while preserving the
 * monotonically increasing private timestamp epoch across the restart. */
#define VPU_STREAM_IDLE_NS	100000000ULL

/* Idle threshold before a random-access picture is suspected to be a seek.
 * Configurable in milliseconds with VPU_STREAM_IDLE_MS for tuning or tests. */
static uint64_t
stream_idle_ns(void)
{
	static uint64_t value;
	static int initialized;

	if (!initialized) {
		const char *env = getenv("VPU_STREAM_IDLE_MS");
		long ms = env && *env ? strtol(env, NULL, 10) :
			(long)(VPU_STREAM_IDLE_NS / 1000000ULL);

		if (ms < 0)
			ms = 0;
		value = (uint64_t)ms * 1000000ULL;
		if (!value)
			value = 1;
		initialized = 1;
	}
	return value;
}

static int g_dbg = -1;

int
vpu_dbg_enabled(void)
{
	if (g_dbg < 0)
		g_dbg = getenv("VPU_VAAPI_DEBUG") != NULL;
	return g_dbg;
}

uint64_t
vpu_monotonic_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

struct vpu_decode_ctx *
vpu_decode_create(const struct vpu_platform *platform)
{
	struct vpu_decode_ctx *ctx = calloc(1, sizeof(*ctx));

	if (ctx) {
		pthread_mutexattr_t attr;

		ctx->session = vpu_platform_session_create(platform);
		if (!ctx->session) {
			free(ctx);
			return NULL;
		}
		ctx->platform_quirks = vpu_platform_quirks(platform);

		pthread_mutexattr_init(&attr);
		pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
		pthread_mutex_init(&ctx->mutex, &attr);
		pthread_mutexattr_destroy(&attr);
		reset_stream_state(ctx);
		ctx->stats_enabled = getenv("VPU_VAAPI_STATS") != NULL;
		ctx->force_h264_sync_end =
			getenv("VPU_H264_SYNC_END") != NULL;
	}
	return ctx;
}

static void
detach_owned_surfaces(struct vpu_decode_ctx *ctx)
{
	int i;

	if (!ctx->surfs)
		return;
	for (i = 0; i < ctx->surfs->n; i++) {
		struct vpu_surface *s = &ctx->surfs->s[i];

		if (s->owner == ctx)
			s->owner = NULL;
	}
}

void
vpu_decode_destroy(struct vpu_decode_ctx *ctx)
{
	if (!ctx)
		return;
	if (ctx->dec_open) {
		/* Complete accepted work on normal context destruction.  Asynchronous
		 * clients such as FFmpeg's null sink may never call vaSyncSurface; a
		 * final EOS drain keeps teardown from silently abandoning in-flight
		 * pictures and makes end-to-end timing cover actual hardware work. */
		if (ctx->dec_started && !ctx->eos_sent && !ctx->fatal_error)
			(void)vpu_decode_flush(ctx);
		finish_pending_writes(ctx);
		vpu_platform_session_close(ctx->session);
	}
	/* Chromium destroys and recreates the VAContext on every seek while
	 * keeping the surface pool.  Abandon the stream epoch *after* the final
	 * drain so the pictures it just completed cannot become the repeat-last-
	 * frame source, and surfaces not re-decoded by the replacement context
	 * cannot leak their pre-seek contents.  VP9 is excluded (retired
	 * predecessor contexts and show_existing_frame rely on cross-context
	 * surfaces). */
	if (ctx->codec != VPU_CODEC_VP9)
		surfs_begin_epoch(ctx->surfs);
	/* Release this context's CAPTURE imports from the display-level Vulkan
	 * cache.  Surfaces outlive their decode context and their destination
	 * buffers are still referenced, but the per-session source keys are not:
	 * leaking them fills the bounded cache and permanently disables the GPU
	 * copy path after enough seeks/navigations. */
	forget_vk_capture_buffers(ctx);
	if (ctx->stats_enabled) {
		double copy_sec = ctx->stats_copy_ns / 1e9;
		double vk_copy_sec = ctx->stats_vk_copy_ns / 1e9;

		fprintf(stderr,
			"[vpu-stats] capture=%llu copied=%llu vk-copy=%llu fallback=%llu direct=%llu copy=%.3fs %.1fGiB/s vk-copy=%.3fs %.1fGiB/s sync=%.3fs rewrite=%llu/%.1fMiB %.3fs end=%llu %.3fs h264-wait=%llu/%.3fs async=%llu\n",
			(unsigned long long)ctx->stats_capture_frames,
			(unsigned long long)ctx->stats_copy_frames,
			(unsigned long long)ctx->stats_vk_copy_frames,
			(unsigned long long)ctx->stats_vk_copy_fallbacks,
			(unsigned long long)ctx->stats_direct_frames,
			copy_sec,
			copy_sec > 0 ? ctx->stats_copy_bytes / copy_sec /
				(1024.0 * 1024.0 * 1024.0) : 0.0,
			vk_copy_sec,
			vk_copy_sec > 0 ? ctx->stats_vk_copy_bytes / vk_copy_sec /
				(1024.0 * 1024.0 * 1024.0) : 0.0,
			ctx->stats_sync_ns / 1e9,
			(unsigned long long)ctx->stats_rewrites,
			ctx->stats_rewrite_bytes / (1024.0 * 1024.0),
			ctx->stats_rewrite_ns / 1e9,
			(unsigned long long)ctx->stats_ends,
			ctx->stats_end_ns / 1e9,
			(unsigned long long)ctx->stats_h264_waits,
			ctx->stats_h264_wait_ns / 1e9,
			(unsigned long long)ctx->stats_h264_async);
	}
	detach_owned_surfaces(ctx);
	vpu_codec_destroy(ctx->codec_adapter);
	vpu_platform_session_destroy(ctx->session);
	pthread_mutex_destroy(&ctx->mutex);
	free(ctx);
}

int
vpu_decode_setup(struct vpu_decode_ctx *ctx, unsigned int width,
		  unsigned int height, VAProfile profile)
{
	ctx->codec_adapter = vpu_codec_create(profile, width, height);
	if (!ctx->codec_adapter)
		return -ENOTSUP;
	ctx->width = width;
	ctx->height = height;
	ctx->codec = vpu_codec_id(ctx->codec_adapter);
	ctx->pixel_format = vpu_codec_pixel_format(ctx->codec_adapter);
	return 0;
}

void
vpu_decode_set_surfaces(struct vpu_decode_ctx *ctx, struct vpu_surfaces *t)
{
	ctx->surfs = t;
	ctx->vk_copy = t ? t->vk_copy : NULL;
}

int
vpu_decode_set_render_targets(struct vpu_decode_ctx *ctx,
			       const VASurfaceID *targets, unsigned int count)
{
	const char *slots_env;
	unsigned int i, direct_count = 0;

	if (!getenv("VPU_DIRECT_CAPTURE") ||
	    !(ctx->platform_quirks & VPU_PLATFORM_QUIRK_HEVC_CAPTURE_FIFO) ||
	    ctx->codec != VPU_CODEC_HEVC)
		return 0;
	if (targets && count >= 4) {
		if (count > ARRAY_SIZE(ctx->direct))
			return -EINVAL;
		for (i = 0; i < count; i++) {
			struct vpu_surface *s = find_surface(ctx, targets[i]);

			if (!s || s->bfd < 0 || s->fourcc != ctx->pixel_format)
				return -EINVAL;
			ctx->direct[i].id = targets[i];
			ctx->direct[i].fd = s->bfd;
			ctx->direct[i].size = s->bsize;
		}
		direct_count = count;
		ctx->direct_requested_count = count;
	} else {
		direct_count = direct_collect_surfaces(ctx);
		/* A dynamic libva pool exposes only one new target at a time.  Its
		 * eventual cycle length cannot be inferred from that first request,
		 * while a stateful decoder needs the complete CAPTURE queue ordering
		 * up front.  Keep the experiment deterministic by requiring its ring
		 * size; fixed-pool clients already supply all render targets above. */
		slots_env = getenv("VPU_DIRECT_CAPTURE_SLOTS");
		if (slots_env && *slots_env) {
			char *end;
			unsigned long slots = strtoul(slots_env, &end, 10);

			if (*end || slots < 4 ||
			    slots > ARRAY_SIZE(ctx->direct))
				return -EINVAL;
			ctx->direct_requested_count = (unsigned int)slots;
			if (direct_count > slots)
				direct_count = (unsigned int)slots;
		} else {
			fprintf(stderr,
				"vpu-vaapi: direct CAPTURE needs a fixed render-target pool; dynamic clients must set VPU_DIRECT_CAPTURE_SLOTS\n");
			return -ENOTSUP;
		}
	}
	ctx->direct_count = direct_count;
	ctx->direct_capture = 1;
	fprintf(stderr,
		"vpu-vaapi: experimental direct CAPTURE ring: %s%u/%u surfaces\n",
		direct_count < ctx->direct_requested_count ? "deferred " : "",
		direct_count, ctx->direct_requested_count);
	return 0;
}

int
vpu_decode_create_surface(struct vpu_decode_ctx *ctx, VASurfaceID id)
{
	/* Legacy entry point kept for local tests: allocate into whatever
	 * registry the engine is attached to. */
	if (!ctx || !ctx->surfs)
		return -1;
	return vpu_surfaces_alloc(ctx->surfs, id, ctx->width, ctx->height,
				 ctx->pixel_format);
}

void
vpu_decode_destroy_surface(struct vpu_decode_ctx *ctx, VASurfaceID id)
{
	if (!ctx || !ctx->surfs)
		return;
	vpu_surfaces_free(ctx->surfs, id);
}

/* Update the coded dimensions of a context that has not started decoding
 * yet (Chrome recreates contexts on resolution changes). */
void
vpu_decode_reconfigure(struct vpu_decode_ctx *ctx, unsigned int width,
			unsigned int height)
{
	if (!ctx || ctx->dec_open || !width || !height)
		return;
	ctx->width = width;
	ctx->height = height;
	vpu_codec_reconfigure(ctx->codec_adapter, width, height);
}

static void
hevc_pending_add(struct vpu_decode_ctx *ctx, int32_t poc, VASurfaceID target,
		 uint64_t generation)
{
	unsigned int n = (unsigned int)ARRAY_SIZE(ctx->hevc_ring);
	unsigned int slot = (ctx->hevc_ring_head + ctx->hevc_ring_len) % n;

	ctx->hevc_ring[slot].poc = poc;
	ctx->hevc_ring[slot].target = target;
	ctx->hevc_ring[slot].generation = generation;
	if (ctx->hevc_ring_len < n) {
		ctx->hevc_ring_len++;
	} else {
		/* Pathologically deep pipeline: drop the oldest mapping
		 * instead of failing the picture submission. */
		ctx->hevc_ring_head = (ctx->hevc_ring_head + 1) % n;
	}
}

static int
hevc_pending_take(struct vpu_decode_ctx *ctx, VASurfaceID *target,
		  uint64_t *generation)
{
	unsigned int n = (unsigned int)ARRAY_SIZE(ctx->hevc_ring);

	if (!ctx->hevc_ring_len)
		return -1;
	*target = ctx->hevc_ring[ctx->hevc_ring_head].target;
	*generation = ctx->hevc_ring[ctx->hevc_ring_head].generation;
	ctx->hevc_ring_head = (ctx->hevc_ring_head + 1) % n;
	ctx->hevc_ring_len--;
	return 0;
}


/* Assign one dequeued frame to its surface. Most paths use the timestamp
 * propagated by firmware. HEVC_CAPTURE_FIFO platforms instead match
 * display-order CAPTURE frames against pending picture POCs.
 * Returns the surface id, or -1 if unknown. */
int
assign_frame(struct vpu_decode_ctx *ctx, const struct vpu_decoded_frame *frame)
{
	VASurfaceID id;
	uint64_t generation;
	struct vpu_surface *s;

	if (frame->bytesused && ctx->stats_enabled)
		ctx->stats_capture_frames++;

	if (ctx->codec == VPU_CODEC_HEVC &&
	    (ctx->platform_quirks & VPU_PLATFORM_QUIRK_HEVC_CAPTURE_FIFO)) {
		if (hevc_pending_take(ctx, &id, &generation)) {
			vpu_platform_session_requeue_index(ctx->session, frame->index);
			return -EIO;
		}
	} else {
		uint64_t frame_seq, slot;

		if (frame->timestamp < 1000000000ULL) {
			vpu_platform_session_requeue_index(ctx->session, frame->index);
			return -EIO;
		}
		frame_seq = (frame->timestamp / 1000000000ULL) - 1000;
		slot = frame_seq % ARRAY_SIZE(ctx->target_ring);
		if (!ctx->target_ring[slot].used ||
		    ctx->target_ring[slot].seq != frame_seq) {
			/* Unknown/stale picture.  The buffer MUST be recycled
			 * or the CAPTURE queue starves and decoding wedges. */
			DBG("[assign] UNKNOWN ts=%llu seq=%llu slot=%llu used=%d rseq=%llu\n",
			    (unsigned long long)frame->timestamp,
			    (unsigned long long)frame_seq,
			    (unsigned long long)slot,
			    ctx->target_ring[slot].used,
			    (unsigned long long)ctx->target_ring[slot].seq);
			vpu_platform_session_requeue_index(ctx->session, frame->index);
			return 0;
		}
		id = ctx->target_ring[slot].target;
		generation = ctx->target_ring[slot].generation;
		ctx->target_ring[slot].used = 0;
	}
	s = find_surface(ctx, id);

	if (!s) {
		DBG("[assign] NO SURFACE id=%u\n", id);
		vpu_platform_session_requeue_index(ctx->session, frame->index);
		return 0;
	}
	/* A VA client may recycle a render target as soon as it drops the old
	 * output frame, while firmware can still have that picture
	 * queued internally (up to its display/decode hold depth).  Do not let
	 * such a late CAPTURE buffer overwrite the newer picture already mapped
	 * to the same stable backing, nor signal the newer picture's fence. */
	if (s->generation != generation) {
		DBG("[assign] STALE id=%u generation=%llu current=%llu\n", id,
		    (unsigned long long)generation,
		    (unsigned long long)s->generation);
		vpu_platform_session_requeue_index(ctx->session, frame->index);
		return 0;
	}
	if (ctx->direct_capture) {
		VASurfaceID actual;

		if (frame->index >= ctx->direct_count) {
			ctx->direct_error = -ERANGE;
			return -1;
		}
		actual = ctx->direct[frame->index].id;
		if (actual != id) {
			fprintf(stderr,
				"direct CAPTURE order mismatch: slot=%u surface=%u expected=%u\n",
				frame->index, actual, id);
			ctx->direct_error = -EIO;
			return -1;
		}
		surface_finish_write(ctx->session, s);
		s->initialized = 1;
		surfaces_mark_decoded(ctx, s);
		ctx->stats_direct_frames++;
		return id;
	}
	/* Copy the decoded frame into the surface's stable DMA-heap backing so
	 * buffers exported before decoding stay valid, then recycle the
	 * firmware buffer. */
	if (frame->bytesused > s->bsize && ctx->codec != VPU_CODEC_VP9) {
		fprintf(stderr,
			"[copy] WARNING frame %u bytes > backing %u; "
			"dropping surface %u\n",
			frame->bytesused, s->bsize, id);
		surface_finish_write(ctx->session, s);
		vpu_platform_session_requeue_index(ctx->session, frame->index);
		return -1;
	} else if (frame->bytesused) {
		unsigned int pitch, width, height;
		int copy_ret;

		vpu_platform_session_capture_layout(ctx->session, &pitch, &width, &height);
		if (pitch != surface_pitch(s->sw, s->fourcc) ||
		    ALIGN_TO(height, 32) != ALIGN_TO(s->sh, 32))
			copy_ret = -ENOTSUP; /* Copy the visible rectangle on the CPU. */
		else
			copy_ret = surface_vk_submit(ctx, s, frame);

		if (copy_ret == 0)
			return id;
		if (copy_ret < 0)
			copy_ret = surface_copy(ctx, s, frame->mem,
						frame->bytesused);
		else
			copy_ret = 0;

		if (copy_ret < 0) {
			fprintf(stderr,
				"[copy] DMA-BUF sync/copy failed for surface %u: %s\n",
				id, strerror(-copy_ret));
			vpu_platform_session_requeue_index(ctx->session, frame->index);
			return -1;
		}
	} else {
		surface_finish_write(ctx->session, s);
	}
	{
		int ret = vpu_platform_session_requeue_index(ctx->session, frame->index);

		if (ret)
			return ret;
	}
	surfaces_mark_decoded(ctx, s);
	return id;
}

static int
vpu_decode_begin_impl(struct vpu_decode_ctx *ctx, VASurfaceID target)
{
	struct vpu_surface *s = find_surface(ctx, target);
	int direct_index = ctx->direct_capture ?
		direct_surface_index(ctx, target) : -1;
	VASurfaceID prev_target = ctx->current_target;
	int field_state;

	if (ctx->direct_error)
		return ctx->direct_error;
	if (ctx->fatal_error)
		return ctx->fatal_error;
	if (!s)
		return -EINVAL;
	/* Never recycle a target while an earlier GPU copy still writes it. */
	if (finish_vk_surface(ctx, target, 1) < 0)
		return -EIO;
	/* Chrome creates its VAContext before it asks for the first surface.  The
	 * opt-in pool preallocation has happened by the first BeginPicture, so bind
	 * the deferred fixed ring here, before V4L2 is opened. */
	if (ctx->direct_capture && direct_index < 0 && !ctx->dec_open) {
		direct_collect_surfaces(ctx);
		if (ctx->direct_count >= ctx->direct_requested_count)
			ctx->direct_count = ctx->direct_requested_count;
		direct_index = direct_surface_index(ctx, target);
	}
	if (ctx->direct_capture && direct_index < 0)
		return -EINVAL;
	if (ctx->direct_capture && ctx->dec_started &&
	    !vpu_platform_session_capture_queued(ctx->session, direct_index)) {
		int ret = vpu_platform_session_requeue_index(ctx->session, direct_index);

		if (ret)
			return ret;
	}

	DBG("[begin] target=%u decoded=%d queued=%d generation=%llu\n",
	    target, s ? s->decoded : -1, s ? s->queued : -1,
	    (unsigned long long)(s ? s->generation : 0));
	/*
	 * PAFF renders the second field of a frame into the same render target
	 * as the first field, right after the first EndPicture.  Do not treat
	 * that as target reuse: keep the generation and the pending frame so
	 * the single firmware frame still matches the first field's ring entry.
	 */
	field_state = vpu_codec_field_state(ctx->codec_adapter);
	if (ctx->field_open && (field_state & 2) && target == prev_target) {
		ctx->second_field = 1;
	} else {
		ctx->second_field = 0;
		ctx->field_open = 0;
		/* VA clients reuse render targets.  A surface that held an
		 * earlier picture must become pending again, otherwise
		 * vaSyncSurface can return the stale backing before the newly
		 * decoded picture is copied into it. */
		s->generation++;
		ctx->current_generation = s->generation;
		s->decoded = 0;
		s->queued = 0;
		/* Do NOT claim the epoch here: a surface only becomes valid for the
		 * current stream when a decoded frame actually lands in it
		 * (surfaces_mark_decoded).  A picture whose CAPTURE frame is dropped
		 * as STALE must stay invalid so vaSyncSurface backfills it instead of
		 * exposing the pre-seek pixels still in the backing. */
	}
	ctx->current_target = target;
	vpu_codec_begin_picture(ctx->codec_adapter);
	return 0;
}

int
vpu_decode_begin(struct vpu_decode_ctx *ctx, VASurfaceID target)
{
	int ret;

	pthread_mutex_lock(&ctx->mutex);
	ret = vpu_decode_begin_impl(ctx, target);
	pthread_mutex_unlock(&ctx->mutex);
	return ret;
}

int
vpu_decode_render(struct vpu_decode_ctx *ctx, VABufferType type,
		   const void *data, size_t size, unsigned int elements)
{
	int ret;

	if (!ctx || !ctx->codec_adapter)
		return -EINVAL;
	pthread_mutex_lock(&ctx->mutex);
	ret = vpu_codec_render(ctx->codec_adapter, type, data, size, elements);
	pthread_mutex_unlock(&ctx->mutex);
	return ret;
}

static int
vpu_decode_end_impl(struct vpu_decode_ctx *ctx)
{
	struct vpu_codec_access_unit codec_unit;
	const uint8_t *au;
	int random_access;
	int vp9_barrier_submitted = 0;
	uint64_t end_start = ctx->stats_enabled ? vpu_monotonic_ns() : 0;
	size_t au_len;
	int ret;

	ret = vpu_codec_build_access_unit(ctx->codec_adapter, &codec_unit);
	if (ret)
		return ret;
	au = codec_unit.data;
	au_len = codec_unit.size;
	random_access = codec_unit.random_access;
	if (ctx->codec == VPU_CODEC_VP9 &&
	    (ctx->platform_quirks & VPU_PLATFORM_QUIRK_VP9_RELEASE_AU)) {
		ret = vpu_codec_build_release_access_unit(ctx->codec_adapter,
				ctx->vp9_release_au, &ctx->vp9_release_len);
		if (ret)
			return ret;
	}
	DBG("[end] target=%u codec=%s au=%zu refs=%d/%d started=%d\n",
	    ctx->current_target, vpu_codec_name(ctx->codec_adapter), au_len,
	    codec_unit.refs_l0, codec_unit.refs_l1, ctx->dec_started);

	/* Stateful firmware may retain pictures from before a Chromium seek because
	 * decoder Reset() has no libva counterpart.  At the first safe random-access
	 * picture that also carries an independent discontinuity signal, drain the
	 * old session through LAST before reopening it, so old and new access units
	 * never coexist in VPU.  Keep the driver's private timestamp sequence
	 * monotonic across the restart. */
	if (ctx->dec_started && random_access) {
		uint64_t now = vpu_monotonic_ns();
		uint64_t gap = ctx->last_submit_ns ? now - ctx->last_submit_ns : 0;
		const char *reason = NULL;

		/* Do not restart on normal in-stream IDRs: some content has a key
		 * frame every few hundred milliseconds.  EOS and a new coded-video
		 * sequence are unambiguous; otherwise the pause before the
		 * random-access picture distinguishes a seek from normal cadence. */
		if (ctx->eos_sent)
			reason = "eos";
		else if (codec_unit.new_sequence)
			reason = "new-sequence";
		else if (gap >= stream_idle_ns())
			reason = "idle";

		if (reason) {
			DBG("[end] stream boundary (%s) gap=%llums codec=0x%x: draining old session\n",
			    reason, (unsigned long long)(gap / 1000000ULL),
			    ctx->codec);
			ret = stream_boundary_restart(ctx);
			if (ret)
				return ret;
			/* Session reset invalidates parameter-set caches.  Rebuild so the
			 * first AU submitted to the new VPU session is self-contained. */
			ret = vpu_codec_build_access_unit(ctx->codec_adapter,
						   &codec_unit);
			if (ret)
				return ret;
			au = codec_unit.data;
			au_len = codec_unit.size;
			random_access = codec_unit.random_access;
		}
	}

	ret = ensure_decoder(ctx);
	if (ret)
		return ret;

	/* Each VPU input submission contains exactly one access unit.  In
	 * particular, do not append an AUD: it starts an empty next access unit
	 * and is invalid for stateful decoders expecting one AU per buffer. */

	/* Opt-in bitstream capture for validating the VA-to-stateful translation
	 * with an independent software decoder. */
	if (ctx->codec == VPU_CODEC_HEVC) {
		const char *dump_path = getenv("VPU_HEVC_DUMP");

		if (dump_path && *dump_path) {
			FILE *dump = fopen(dump_path, "ab");

			if (dump) {
				fwrite(au, 1, au_len, dump);
				fclose(dump);
			}
		}
	} else if (ctx->codec == VPU_CODEC_H264) {
		const char *dump_path = getenv("VPU_H264_DUMP");

		if (dump_path && *dump_path) {
			FILE *dump = fopen(dump_path, "ab");

			if (dump) {
				fwrite(au, 1, au_len, dump);
				fclose(dump);
			}
		}
	}

	{
		uint64_t ts;

		if (ctx->second_field) {
			/* Second PAFF field: reuse the first field's timestamp and
			 * keep its target ring entry and pending fence; the pair
			 * is one firmware frame. */
			ts = ctx->field_ts;
		} else {
			ts = (ctx->seq + 1000) * 1000000000ULL;
			ctx->last_target = ctx->current_target;
			{
				struct vpu_surface *qt = find_surface(ctx,
								       ctx->current_target);

				if (qt) {
					uint64_t token = ctx->seq + 1;

					qt->queued = 1;
					qt->owner = ctx;
					if (ctx->direct_capture ||
					    (ctx->vk_copy && !ctx->vk_copy_failed))
						surface_begin_device_write(ctx->session, qt, token);
					else
						surface_begin_write(ctx->session, qt, token);
				}
			}
			/* Ring mapping: only a handful of frames (bounded by the
			 * CAPTURE buffer count) are ever in flight, so a slot is
			 * reused long after its previous frame was dequeued.  seq
			 * itself keeps growing so timestamps stay unique. */
			{
				unsigned int slot = (unsigned int)(ctx->seq %
					ARRAY_SIZE(ctx->target_ring));

				ctx->target_ring[slot].seq = ctx->seq;
				ctx->target_ring[slot].target = ctx->current_target;
				ctx->target_ring[slot].generation =
					ctx->current_generation;
				ctx->target_ring[slot].used = 1;
			}
		}

		if (!ctx->dec_started) {
			/* Mirror FFmpeg: queue the first access unit before
			 * STREAMON, otherwise the firmware rejects the CAPTURE
			 * setup (0x1004). */
			ret = vpu_platform_session_submit(ctx->session, au, au_len, ts);
			if (ret) {
				finish_pending_writes(ctx);
				return ret;
			}
			ret = vpu_platform_session_start(ctx->session);
			if (ret) {
				finish_pending_writes(ctx);
				return ret;
			}
			ctx->dec_started = 1;
		} else {
			/* Wait for a free OUTPUT buffer, then queue.  The firmware
			 * stops consuming input when its CAPTURE queue fills, so
			 * drain finished frames while we wait or we deadlock. */
			for (int spin = 0; spin < 100; spin++) {
				while (vpu_platform_session_dequeue_input(ctx->session) == 0)
					;
				ret = vpu_platform_session_submit(ctx->session, au, au_len, ts);
				if (ret != -EAGAIN)
					break;
				ret = drain_available(ctx);
				if (ret)
					break;
				vpu_platform_session_poll(ctx->session, 50);
			}
			if (ret) {
				finish_pending_writes(ctx);
				return ret;
			}
		}
		if (ctx->codec == VPU_CODEC_HEVC &&
		    (ctx->platform_quirks &
		     VPU_PLATFORM_QUIRK_HEVC_CAPTURE_FIFO))
			hevc_pending_add(ctx,
					 codec_unit.picture_order_count,
					 ctx->current_target,
					 ctx->current_generation);
		/*
		 * Track PAFF field pairs.  The first field opens the pair and
		 * owns the target entry and fence; the second field closes it.
		 * The pair shares one sequence number, so a frame advances seq
		 * exactly once.
		 */
		{
			int field_state = vpu_codec_field_state(ctx->codec_adapter);

			if (field_state & 1) {
				if (ctx->second_field) {
					ctx->field_open = 0;
				} else {
					ctx->field_open = 1;
					ctx->field_ts = ts;
				}
			} else {
				ctx->field_open = 0;
			}
		}
		if (!ctx->second_field)
			ctx->seq++;
	}
	/* Legacy VPU5 retains the current VP9 picture until another access unit
	 * arrives.  Chrome may present an exported key-frame target immediately
	 * after EndPicture and does not reliably observe a reservation fence added
	 * after the DMA-BUF was imported.  Queue one internal duplicate key frame
	 * to release the real target, then wait briefly for that real completion.
	 * The duplicate has its own timestamp but no target-ring mapping, so its
	 * eventual CAPTURE output is recycled rather than exposed to Chrome. */
	if (ctx->codec == VPU_CODEC_VP9 && random_access &&
	    (ctx->platform_quirks & VPU_PLATFORM_QUIRK_VP9_RELEASE_AU)) {
		uint64_t prime_ts = (ctx->seq + 1000) * 1000000000ULL;

		for (int spin = 0; spin < 100; spin++) {
			while (vpu_platform_session_dequeue_input(ctx->session) == 0)
				;
			ret = vpu_platform_session_submit(ctx->session, au, au_len, prime_ts);
			if (ret != -EAGAIN)
				break;
			ret = drain_available(ctx);
			if (ret)
				break;
			vpu_platform_session_poll(ctx->session, 50);
		}
		if (ret)
			return ret;
		DBG("[vp9-keyframe] prime seq=%llu target=%u\n",
		    (unsigned long long)ctx->seq, ctx->current_target);
		ctx->seq++;
	}
	/* The seek key frame above is complete before EndPicture returns, but VPU5
	 * would hold the immediately following real inter frame.  ANGLE can sample
	 * its exported target before the next Chrome decode call and briefly expose
	 * the target's pre-seek pixels.  One show_existing_frame AU pushes that first
	 * inter frame out without decoding a duplicate or changing VP9 references. */
	if (ctx->codec == VPU_CODEC_VP9 &&
	    (ctx->platform_quirks & VPU_PLATFORM_QUIRK_VP9_RELEASE_AU) &&
	    ctx->vp9_seek_barrier && !random_access) {
		uint8_t show_existing[2];
		size_t show_existing_len;
		uint64_t barrier_ts = (ctx->seq + 1000) * 1000000000ULL;

		ret = vpu_codec_build_release_access_unit(ctx->codec_adapter,
						   show_existing,
						   &show_existing_len);
		if (ret)
			return ret;
		for (int spin = 0; spin < 100; spin++) {
			while (vpu_platform_session_dequeue_input(ctx->session) == 0)
				;
			ret = vpu_platform_session_submit(ctx->session, show_existing,
					    show_existing_len, barrier_ts);
			if (ret != -EAGAIN)
				break;
			ret = drain_available(ctx);
			if (ret)
				break;
			vpu_platform_session_poll(ctx->session, 50);
		}
		if (ret)
			return ret;
		DBG("[vp9-seek] inter barrier seq=%llu target=%u\n",
		    (unsigned long long)ctx->seq, ctx->current_target);
		ctx->seq++;
		ctx->vp9_seek_barrier = 0;
		vp9_barrier_submitted = 1;
	}
	/* The stateful firmware holds each frame until the next access unit
	 * arrives.  Draining right after this feed makes the *previous*
	 * picture's frame available so a client that syncs one picture at a
	 * time (ffmpeg/Chrome) does not deadlock. */
	ret = drain_available(ctx);
	if (ret)
		return ret;
	if (ctx->codec == VPU_CODEC_VP9 &&
	    (ctx->platform_quirks & VPU_PLATFORM_QUIRK_VP9_RELEASE_AU) &&
	    (random_access || vp9_barrier_submitted)) {
		ret = wait_surface_ready(ctx, ctx->current_target, 20);
		if (ret) {
			DBG("[vp9-barrier] target=%u wait incomplete: %d\n",
			    ctx->current_target, ret);
			/* A seek boundary is specifically protecting ANGLE from stale
			 * pixels.  Prefer decoder fallback to returning that surface
			 * before it is ready. */
			if (ctx->vp9_seek_barrier || vp9_barrier_submitted)
				return ret;
		}
	}

	/* Chrome sends an already-exported surface to ANGLE immediately after
	 * vaEndPicture.  Legacy Adreno can sample its previous contents even though
	 * a new reservation fence was attached.  Asynchronous HEVC let Chrome
	 * recycle a target one or two generations before its CAPTURE frame arrived,
	 * so H.264 and HEVC exported targets need backpressure here.  VP9 normally
	 * stays asynchronous because its firmware holds the current picture until
	 * the next frame; its safe key-frame and post-seek barriers are handled
	 * above only after an internal AU has pushed the target out. */
	{
		struct vpu_surface *target = find_surface(ctx,
							   ctx->current_target);
		int wait_exported_codec = ctx->codec == VPU_CODEC_H264 ||
			ctx->codec == VPU_CODEC_HEVC;
		int first_field = (vpu_codec_field_state(ctx->codec_adapter) & 1) &&
			!ctx->second_field;
		/* The first field of a pair cannot complete until the second is
		 * submitted, so never block on an exported target here. */
		int exported_wait = wait_exported_codec && target &&
			target->exported && !first_field;
		int forced_wait = ctx->codec == VPU_CODEC_H264 &&
			ctx->force_h264_sync_end;

		if (exported_wait || forced_wait) {
			uint64_t wait_start = ctx->stats_enabled ? vpu_monotonic_ns() : 0;

			DBG("[end] waiting exported target=%u codec=0x%x\n",
			    ctx->current_target, ctx->codec);
			ret = wait_surface_ready(ctx, ctx->current_target, 100);
			if (ctx->stats_enabled &&
			    ctx->codec == VPU_CODEC_H264) {
				ctx->stats_h264_wait_ns += vpu_monotonic_ns() - wait_start;
				ctx->stats_h264_waits++;
			}
			if (ret) {
				DBG("[end] target=%u readiness wait failed: %d\n",
				    ctx->current_target, ret);
				return ret;
			}
			DBG("[end] exported target=%u ready\n",
			    ctx->current_target);
		} else if (ctx->stats_enabled &&
			   ctx->codec == VPU_CODEC_H264) {
			ctx->stats_h264_async++;
		}
	}
	vpu_codec_finish_picture(ctx->codec_adapter);
	ctx->last_submit_ns = vpu_monotonic_ns();
	if (ctx->stats_enabled) {
		ctx->stats_rewrite_ns += codec_unit.rewrite_ns;
		ctx->stats_rewrite_bytes += codec_unit.rewrite_bytes;
		ctx->stats_rewrites += codec_unit.rewrites;
		ctx->stats_end_ns += vpu_monotonic_ns() - end_start;
		ctx->stats_ends++;
	}
	DBG("[end] done rv=0\n");
	return 0;
}

int
vpu_decode_end(struct vpu_decode_ctx *ctx)
{
	int ret;

	pthread_mutex_lock(&ctx->mutex);
	ret = vpu_decode_end_impl(ctx);
	pthread_mutex_unlock(&ctx->mutex);
	return ret;
}

static int
vpu_decode_surface_ready(struct vpu_decode_ctx *ctx, VASurfaceID id)
{
	if (drain_available(ctx))
		return 0;
	if (finish_vk_surface(ctx, id, 0) < 0)
		return 0;
	{
		struct vpu_surface *s = find_surface(ctx, id);

		return s ? s->decoded : 0;
	}
}

static int
vpu_decode_sync_impl(struct vpu_decode_ctx *ctx, VASurfaceID id)
{
	struct vpu_surface *s = find_surface(ctx, id);
	int deadline = 200;	/* ~2 s */

	/* Chrome preallocates a surface pool and syncs each freshly created
	 * (never-decoded) surface before exporting it.  The backing buffer is
	 * already valid, so an unqueued surface syncs immediately. */
	if (!s)
		return -EINVAL;
	if (ctx->fatal_error)
		return ctx->fatal_error;
	if (!s->queued)
		return 0;

	if (vpu_decode_surface_ready(ctx, id))
		return 0;
	if (ctx->fatal_error)
		return ctx->fatal_error;

	/* Sync can follow every submission (for example FFmpeg with one
	 * decoder thread). Decode-order H.264/HEVC output can complete without
	 * EOS; draining here would discard the references needed by the next
	 * inter picture when ensure_decoder reopens the session.
	 */
	if (!ctx->eos_sent &&
	    (ctx->codec == VPU_CODEC_H264 || ctx->codec == VPU_CODEC_HEVC))
		return wait_surface_ready(ctx, id, 100);

	/* Release the held VP9 picture without ending its reference lifetime.
	 * This internal show-existing AU has no VA target mapping.
	 */
	if (!ctx->eos_sent && id == ctx->last_target &&
	    ctx->codec == VPU_CODEC_VP9 && ctx->vp9_release_len &&
	    (ctx->platform_quirks & VPU_PLATFORM_QUIRK_VP9_RELEASE_AU)) {
		int ret = -EAGAIN;
		uint64_t ts = (ctx->seq + 1000) * 1000000000ULL;

		for (int spin = 0; spin < 100; spin++) {
			while (vpu_platform_session_dequeue_input(ctx->session) == 0)
				;
			ret = vpu_platform_session_submit(ctx->session,
				ctx->vp9_release_au, ctx->vp9_release_len, ts);
			if (ret != -EAGAIN)
				break;
			ret = drain_available(ctx);
			if (ret)
				return ret;
			vpu_platform_session_poll(ctx->session, 20);
			ret = -EAGAIN;
		}
		if (ret)
			return ret;
		ctx->seq++;
		return wait_surface_ready(ctx, id, 100);
	}

	/* The stateful firmware holds the last queued picture until an EOS
	 * marker arrives.  If the client is syncing that final picture, feed
	 * EOS to force it out instead of spinning until the timeout. */
	if (id == ctx->last_target && !ctx->eos_sent) {
		DBG("[sync] last_target=%u: flushing\n", id);
		if (vpu_decode_flush(ctx) == 0 &&
		    vpu_decode_surface_ready(ctx, id))
			return 0;
	}

	while (deadline-- > 0) {
		struct vpu_decoded_frame frame;
		int changed, ret;

		/* Wait for real CAPTURE progress; POLLOUT would wake us
		 * instantly and burn the deadline before any frame is done. */
		ret = vpu_platform_session_poll_capture(ctx->session, 20);
		if (ret < 0) {
			ctx->fatal_error = ret;
			return ret;
		}
		if (!ret)
			continue;
		(void)changed;
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
		DBG("[sync] got ts=%llu\n",
		    (unsigned long long)frame.timestamp);
		ret = assign_frame(ctx, &frame);
		if (ret < 0) {
			ctx->fatal_error = ret;
			return ret;
		}
		if (vpu_decode_surface_ready(ctx, id))
			return 0;
	}
	return -ETIMEDOUT;
}

int
vpu_decode_sync(struct vpu_decode_ctx *ctx, VASurfaceID id)
{
	int ret;

	pthread_mutex_lock(&ctx->mutex);
	ret = vpu_decode_sync_impl(ctx, id);
	pthread_mutex_unlock(&ctx->mutex);
	return ret;
}
