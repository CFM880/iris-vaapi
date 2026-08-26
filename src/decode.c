// SPDX-License-Identifier: GPL-2.0-or-later

#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>

#include "decode.h"
#include "h264_params.h"
#include "hevc_params.h"
#include "hevc_slice_rewrite.h"
#include "v4l2_dec.h"

/* Chrome runs one VaapiVideoDecoder per video; every decoder owns a frame
 * pool of up to ~32 surfaces and pools coexist across tabs/resolution
 * changes, so the registry must hold several pools at once. */
#define IRIS_MAX_SURFACES	128
#define IRIS_AU_MAX		(16U * 1024 * 1024)

#ifndef ALIGN_TO
#define ALIGN_TO(x, a) (((x) + (a) - 1) & ~((a) - 1))
#endif
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

static unsigned int
surface_pitch(unsigned int width, unsigned int fourcc)
{
	unsigned int bytes = fourcc == V4L2_PIX_FMT_P010 ? 2 : 1;
	unsigned int alignment = fourcc == V4L2_PIX_FMT_P010 ? 256 : 128;

	return ALIGN_TO(width * bytes, alignment);
}

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC	0x0001U
#endif

/* Per-frame tracing is extremely chatty and the GPU process inherits this
 * stderr; unconditionally writing it stalls the decode loop when the terminal
 * is slow.  Opt in with IRIS_VAAPI_DEBUG=1. */
static int g_dbg = -1;

static int dbg_enabled(void)
{
	if (g_dbg < 0)
		g_dbg = getenv("IRIS_VAAPI_DEBUG") != NULL;
	return g_dbg;
}

#define DBG(...)	do { if (dbg_enabled()) fprintf(stderr, __VA_ARGS__); } while (0)

static uint64_t
monotonic_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int
dma_heap_alloc(int heap_fd, unsigned int size)
{
	struct dma_heap_allocation_data data;

	memset(&data, 0, sizeof(data));
	data.len = size;
	data.fd = 0;
	data.fd_flags = O_RDWR | O_CLOEXEC;
	if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &data) < 0)
		return -1;
	return data.fd;
}

/* Fallback backing for when /dev/dma_heap/system is not accessible (root
 * only): an anonymous memfd is mmappable and readable by local tests and the
 * ffmpeg CPU readback path, but is NOT a DRM buffer and cannot be imported by
 * EGL/GPU clients like Chrome. */
static int
memfd_alloc(unsigned int size)
{
	int fd = (int)syscall(SYS_memfd_create, "iris-surface", MFD_CLOEXEC);

	if (fd < 0)
		return -1;
	if (ftruncate(fd, size) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int
dma_buf_cpu_sync(int fd, uint64_t flags)
{
	struct dma_buf_sync sync = { .flags = flags };
	int ret;

	do {
		ret = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
	} while (ret < 0 && errno == EINTR);
	return ret;
}

struct iris_surface {
	VASurfaceID id;
	int bfd;		/* backing fd (DMA-heap, or memfd fallback) */
	void *bmap;		/* mmap of the backing */
	unsigned int bsize;
	unsigned int sw, sh;	/* coded size the backing was created for */
	unsigned int fourcc;	/* VA/V4L2 layout: NV12 or P010 */
	int decoded;		/* a frame has been copied into the backing */
	int queued;		/* some picture was decoded into this surface */
	int write_started;	/* DMA_BUF_SYNC write access spans async decode */
	uint64_t fence_token;	/* pending kernel reservation fence, or zero */
	uint64_t generation;	/* render-target reuse generation */
	struct iris_decode_ctx *owner;	/* engine that queued the picture */
};

static int
surface_finish_write(struct v4l2_dec *dec, struct iris_surface *s)
{
	int ret = 0;

	if (s->write_started &&
	    dma_buf_cpu_sync(s->bfd,
			     DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE) < 0)
		ret = -errno;
	s->write_started = 0;
	if (s->fence_token) {
		int signal_ret = v4l2_dec_signal_surface_fence(dec,
							 s->fence_token);

		s->fence_token = 0;
		if (!ret && signal_ret)
			ret = signal_ret;
	}
	return ret;
}

static void
surface_begin_write(struct v4l2_dec *dec, struct iris_surface *s,
		    uint64_t token)
{
	if (s->write_started || s->fence_token)
		surface_finish_write(dec, s);
	if (dma_buf_cpu_sync(s->bfd,
			     DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE) < 0)
		return;
	s->write_started = 1;
	if (!v4l2_dec_attach_surface_fence(dec, s->bfd, token)) {
		s->fence_token = token;
	} else {
		dma_buf_cpu_sync(s->bfd,
				 DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
		s->write_started = 0;
	}
}

static void
surface_begin_device_write(struct v4l2_dec *dec, struct iris_surface *s,
			   uint64_t token)
{
	if (s->write_started || s->fence_token)
		surface_finish_write(dec, s);
	if (!v4l2_dec_attach_surface_fence(dec, s->bfd, token))
		s->fence_token = token;
}

/* Display-level registry: surfaces may outlive the engine that decodes into
 * them (Chrome destroys contexts on navigation while pool surfaces drain). */
struct iris_surfs {
	struct iris_surface s[IRIS_MAX_SURFACES];
	int n;
};

struct iris_decode_ctx {
	struct v4l2_dec dec;
	int dec_open;
	int dec_started;
	unsigned int width, height;
	VAProfile profile;
	unsigned int out_pixfmt;	/* V4L2 OUTPUT pixel format */
	unsigned int cap_pixfmt;	/* V4L2 CAPTURE pixel format */
	int direct_capture;
	int direct_error;
	int fatal_error;
	unsigned int direct_count;
	unsigned int direct_requested_count;
	struct {
		VASurfaceID id;
		int fd;
		size_t size;
	} direct[IRIS_MAX_SURFACES];

	struct iris_surfs *surfs;	/* not owned */

	VAPictureParameterBufferH264 pic;
	int have_pic;
	VAIQMatrixBufferH264 h264_iq;
	int have_h264_iq;
	VAPictureParameterBufferHEVC hevc_pic;
	int have_hevc_pic;
	VAIQMatrixBufferHEVC hevc_iq;
	int have_hevc_iq;
	uint8_t *slice_data;
	size_t slice_len;
	size_t slice_cap;
	uint8_t *au_data;
	size_t au_cap;
	VASurfaceID current_target;
	uint64_t current_generation;

	uint8_t last_sps[256];
	int last_sps_len;
	uint8_t last_pps[4096];
	int last_pps_len;
	/* HEVC parameter-set cache (only emit when changed). */
	uint8_t last_hevc_vps[64];
	int last_hevc_vps_len;
	uint8_t last_hevc_sps[8192];
	int last_hevc_sps_len;
	uint8_t last_hevc_pps[128];
	int last_hevc_pps_len;
	/* Original parameter NALs, when the VA client supplies them in the
	 * slice-data buffer.  Stateful V4L2 needs these bytes verbatim. */
	uint8_t raw_hevc_vps[1024];
	uint8_t raw_hevc_sps[1024];
	uint8_t raw_hevc_pps[1024];
	int raw_hevc_vps_len;
	int raw_hevc_sps_len;
	int raw_hevc_pps_len;
	int refs_l0, refs_l1;	/* effective per-slice reference counts */
	int h264_pps_refs_l0, h264_pps_refs_l1;
	struct {
		VASliceParameterBufferH264 param;
	} h264_slices[128];
	unsigned int h264_slice_count;
	struct {
		VASliceParameterBufferHEVC param;
	} hevc_slices[128];
	unsigned int hevc_slice_count;
	unsigned int hevc_slice_next;
	int hevc_pps_id;
	int hevc_rewritten;

	/* Map decode sequence numbers back to target surfaces so frame
	 * matching does not depend on the (possibly non-contiguous) VASurfaceID
	 * values that the client happens to use.  The ring is indexed by
	 * (seq & mask) and validated by the stored seq, so playback longer
	 * than any fixed table just wraps instead of breaking. */
#define IRIS_TARGET_RING	1024	/* power of two */
	uint64_t seq;
	struct {
		uint64_t seq;
		VASurfaceID target;
		uint64_t generation;
		int used;
	} target_ring[IRIS_TARGET_RING];
	VASurfaceID last_target;	/* most recently queued picture */
	int eos_sent;			/* EOS (v4l2_dec_flush) queued */
	/* HEVC firmware does not propagate usable per-frame timestamps.  Iris
	 * emits CAPTURE frames in display order, so retain POC-to-surface
	 * state as a strict FIFO ring: pictures complete in the order they
	 * were queued, and a ring can never overflow into a hard failure. */
#define IRIS_HEVC_RING	512
	struct {
		int32_t poc;
		VASurfaceID target;
		uint64_t generation;
	} hevc_ring[IRIS_HEVC_RING];
	unsigned int hevc_ring_head, hevc_ring_len;

	int stats_enabled;
	uint64_t stats_copy_ns;
	uint64_t stats_copy_bytes;
	uint64_t stats_copy_frames;
	uint64_t stats_sync_ns;
	uint64_t stats_rewrite_ns;
	uint64_t stats_rewrite_bytes;
	uint64_t stats_rewrites;
	uint64_t stats_end_ns;
	uint64_t stats_ends;
	uint64_t stats_direct_frames;
};

static int
surface_copy(struct iris_decode_ctx *ctx, struct iris_surface *s,
	     const void *src, size_t size)
{
	int sync_started = s->write_started;
	int ret = 0;
	uint64_t start;

	if (size > s->bsize)
		return -E2BIG;
	/* The backing is imported by Chrome's GPU process while this process
	 * updates it through an mmap.  DMA_BUF_IOCTL_SYNC supplies the required
	 * ownership/cache transition on non-coherent ARM systems; without it,
	 * 4K frames can be sampled with stale cache lines and appear torn or
	 * partially corrupted.  memfd fallback buffers do not support this
	 * ioctl and remain ordinary coherent CPU mappings. */
	if (!sync_started && dma_buf_cpu_sync(s->bfd,
				    DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE) == 0) {
		s->write_started = 1;
		sync_started = 1;
	} else if (!sync_started && errno != ENOTTY && errno != EINVAL) {
		return -errno;
	}

	start = ctx->stats_enabled ? monotonic_ns() : 0;
	memcpy(s->bmap, src, size);
	if (ctx->stats_enabled) {
		ctx->stats_copy_ns += monotonic_ns() - start;
		ctx->stats_copy_bytes += size;
		ctx->stats_copy_frames++;
	}

	if (sync_started) {
		start = ctx->stats_enabled ? monotonic_ns() : 0;
		ret = surface_finish_write(&ctx->dec, s);
		if (ctx->stats_enabled)
			ctx->stats_sync_ns += monotonic_ns() - start;
	}
	return ret;
}

static void
finish_pending_writes(struct iris_decode_ctx *ctx)
{
	int i;

	if (!ctx->surfs || !ctx->dec_open)
		return;
	for (i = 0; i < ctx->surfs->n; i++) {
		struct iris_surface *s = &ctx->surfs->s[i];

		if (s->owner == ctx && (s->write_started || s->fence_token))
			surface_finish_write(&ctx->dec, s);
	}
}

static void
detach_owned_surfaces(struct iris_decode_ctx *ctx)
{
	int i;

	if (!ctx->surfs)
		return;
	for (i = 0; i < ctx->surfs->n; i++) {
		struct iris_surface *s = &ctx->surfs->s[i];

		if (s->owner == ctx)
			s->owner = NULL;
	}
}

static struct iris_surface *
surfs_find(struct iris_surfs *t, VASurfaceID id)
{
	int i;

	if (!t)
		return NULL;
	for (i = 0; i < t->n; i++)
		if (t->s[i].id == id)
			return &t->s[i];
	return NULL;
}

static struct iris_surface *
find_surface(struct iris_decode_ctx *ctx, VASurfaceID id)
{
	return surfs_find(ctx->surfs, id);
}

static void
target_ring_reset(struct iris_decode_ctx *ctx)
{
	memset(ctx->target_ring, 0, sizeof(ctx->target_ring));
}

/* Clear all per-stream decode state: parameter-set caches, the sequence to
 * surface mapping and EOS bookkeeping.  Surfaces and their backings are
 * preserved so clients may keep exporting them. */
static void
reset_stream_state(struct iris_decode_ctx *ctx)
{
	ctx->dec_started = 0;
	ctx->eos_sent = 0;
	ctx->last_target = 0;
	ctx->have_pic = 0;
	ctx->have_h264_iq = 0;
	ctx->have_hevc_pic = 0;
	ctx->have_hevc_iq = 0;
	ctx->slice_len = 0;
	ctx->refs_l0 = 0;
	ctx->refs_l1 = 0;
	ctx->h264_pps_refs_l0 = -1;
	ctx->h264_pps_refs_l1 = -1;
	ctx->h264_slice_count = 0;
	ctx->hevc_slice_count = 0;
	ctx->hevc_slice_next = 0;
	ctx->hevc_pps_id = -1;
	ctx->hevc_rewritten = 0;
	ctx->last_sps_len = 0;
	ctx->last_pps_len = 0;
	ctx->last_hevc_vps_len = 0;
	ctx->last_hevc_sps_len = 0;
	ctx->last_hevc_pps_len = 0;
	ctx->raw_hevc_vps_len = 0;
	ctx->raw_hevc_sps_len = 0;
	ctx->raw_hevc_pps_len = 0;
	ctx->hevc_ring_head = 0;
	ctx->hevc_ring_len = 0;
	ctx->direct_error = 0;
	ctx->fatal_error = 0;
	target_ring_reset(ctx);
	ctx->seq = 0;
}

struct iris_decode_ctx *
iris_decode_create(void)
{
	struct iris_decode_ctx *ctx = calloc(1, sizeof(*ctx));

	if (ctx) {
		reset_stream_state(ctx);
		ctx->stats_enabled = getenv("IRIS_VAAPI_STATS") != NULL;
	}
	return ctx;
}

void
iris_decode_destroy(struct iris_decode_ctx *ctx)
{
	if (!ctx)
		return;
	if (ctx->dec_open) {
		finish_pending_writes(ctx);
		v4l2_dec_close(&ctx->dec);
	}
	if (ctx->stats_enabled) {
		double copy_sec = ctx->stats_copy_ns / 1e9;

		fprintf(stderr,
			"[iris-stats] frames=%llu direct=%llu copy=%.3fs %.1fGiB/s sync=%.3fs rewrite=%llu/%.1fMiB %.3fs end=%llu %.3fs\n",
			(unsigned long long)ctx->stats_copy_frames,
			(unsigned long long)ctx->stats_direct_frames,
			copy_sec,
			copy_sec > 0 ? ctx->stats_copy_bytes / copy_sec /
				(1024.0 * 1024.0 * 1024.0) : 0.0,
			ctx->stats_sync_ns / 1e9,
			(unsigned long long)ctx->stats_rewrites,
			ctx->stats_rewrite_bytes / (1024.0 * 1024.0),
			ctx->stats_rewrite_ns / 1e9,
			(unsigned long long)ctx->stats_ends,
			ctx->stats_end_ns / 1e9);
	}
	detach_owned_surfaces(ctx);
	free(ctx->slice_data);
	free(ctx->au_data);
	free(ctx);
}

void
iris_decode_setup(struct iris_decode_ctx *ctx, unsigned int width,
		  unsigned int height, VAProfile profile)
{
	ctx->width = width;
	ctx->height = height;
	ctx->profile = profile;
	ctx->cap_pixfmt = profile == VAProfileHEVCMain10 ||
		profile == VAProfileVP9Profile2 ? V4L2_PIX_FMT_P010 :
		V4L2_PIX_FMT_NV12;
	switch (profile) {
	case VAProfileHEVCMain:
	case VAProfileHEVCMain10:
		ctx->out_pixfmt = V4L2_PIX_FMT_HEVC;
		break;
	case VAProfileVP9Profile0:
	case VAProfileVP9Profile1:
	case VAProfileVP9Profile2:
	case VAProfileVP9Profile3:
		ctx->out_pixfmt = V4L2_PIX_FMT_VP9;
		break;
	default:
		ctx->out_pixfmt = V4L2_PIX_FMT_H264;
		break;
	}
}

void
iris_decode_set_surfaces(struct iris_decode_ctx *ctx, struct iris_surfs *t)
{
	ctx->surfs = t;
}

static unsigned int
direct_collect_surfaces(struct iris_decode_ctx *ctx)
{
	unsigned int i, count = 0;

	for (i = 0; i < (unsigned int)ctx->surfs->n &&
	     count < ARRAY_SIZE(ctx->direct); i++) {
		struct iris_surface *s = &ctx->surfs->s[i];

		if (s->sw != ctx->width || s->sh != ctx->height ||
		    s->fourcc != ctx->cap_pixfmt || s->bfd < 0 ||
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
iris_decode_set_render_targets(struct iris_decode_ctx *ctx,
			       const VASurfaceID *targets, unsigned int count)
{
	const char *slots_env;
	unsigned int i, direct_count = 0;

	if (!getenv("IRIS_DIRECT_CAPTURE") ||
	    ctx->out_pixfmt != V4L2_PIX_FMT_HEVC)
		return 0;
	if (targets && count >= 4) {
		if (count > ARRAY_SIZE(ctx->direct))
			return -EINVAL;
		for (i = 0; i < count; i++) {
			struct iris_surface *s = find_surface(ctx, targets[i]);

			if (!s || s->bfd < 0 || s->fourcc != ctx->cap_pixfmt)
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
		slots_env = getenv("IRIS_DIRECT_CAPTURE_SLOTS");
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
				"iris-vaapi: direct CAPTURE needs a fixed render-target pool; dynamic clients must set IRIS_DIRECT_CAPTURE_SLOTS\n");
			return -ENOTSUP;
		}
	}
	ctx->direct_count = direct_count;
	ctx->direct_capture = 1;
	fprintf(stderr,
		"iris-vaapi: experimental direct CAPTURE ring: %s%u/%u surfaces\n",
		direct_count < ctx->direct_requested_count ? "deferred " : "",
		direct_count, ctx->direct_requested_count);
	return 0;
}

int
iris_decode_create_surface(struct iris_decode_ctx *ctx, VASurfaceID id)
{
	/* Legacy entry point kept for local tests: allocate into whatever
	 * registry the engine is attached to. */
	if (!ctx || !ctx->surfs)
		return -1;
	return iris_surfs_alloc(ctx->surfs, id, ctx->width, ctx->height,
				 ctx->cap_pixfmt);
}

void
iris_decode_destroy_surface(struct iris_decode_ctx *ctx, VASurfaceID id)
{
	if (!ctx || !ctx->surfs)
		return;
	iris_surfs_free(ctx->surfs, id);
}

/* Update the coded dimensions of a context that has not started decoding
 * yet (Chrome recreates contexts on resolution changes). */
void
iris_decode_reconfigure(struct iris_decode_ctx *ctx, unsigned int width,
			unsigned int height)
{
	if (!ctx || ctx->dec_open || !width || !height)
		return;
	ctx->width = width;
	ctx->height = height;
}

/* Tear down the firmware session and all stream state (Chrome Flush/Reset,
 * seeks).  Surfaces and their backings are preserved so frames already
 * exported to the client stay valid. */
void
iris_decode_reset(struct iris_decode_ctx *ctx)
{
	if (!ctx)
		return;
	if (ctx->dec_open) {
		finish_pending_writes(ctx);
		v4l2_dec_close(&ctx->dec);
		ctx->dec_open = 0;
	}
	reset_stream_state(ctx);
}

/* ---- Display-level surface registry ---- */

struct iris_surfs *
iris_surfs_create(void)
{
	return calloc(1, sizeof(struct iris_surfs));
}

void
iris_surfs_destroy(struct iris_surfs *t)
{
	int i;

	if (!t)
		return;
	for (i = 0; i < t->n; i++) {
		struct iris_surface *s = &t->s[i];

		if (s->owner && s->owner->dec_open &&
		    (s->write_started || s->fence_token))
			surface_finish_write(&s->owner->dec, s);
		munmap(t->s[i].bmap, t->s[i].bsize);
		close(t->s[i].bfd);
	}
	free(t);
}

int
iris_surfs_alloc(struct iris_surfs *t, VASurfaceID id,
		 unsigned int width, unsigned int height, unsigned int fourcc)
{
	struct iris_surface *s;
	unsigned int size;
	int heap, bfd;
	void *map;

	if (!t || t->n >= IRIS_MAX_SURFACES)
		return -1;

	/* Stable, exportable backing buffer independent of any V4L2 session.
	 * Prefer a real DMA-heap buffer so the exported fd can be imported by
	 * GPU clients (Chrome/EGL); fall back to a plain memfd when the heap
	 * node is root-only, which keeps local tests and CPU readback working.
	 * Size with the layout Iris produces for linear NV12/P010 (128-byte
	 * NV12 stride, 256-byte P010 stride, 32-aligned luma height). */
	size = surface_pitch(width, fourcc) * ALIGN_TO(height, 32) * 3 / 2;
	DBG("[surf] id=%u size=%u w=%u h=%u fourcc=%#x\n",
	    id, size, width, height, fourcc);
	heap = open("/dev/dma_heap/system", O_RDWR);
	if (heap >= 0) {
		bfd = dma_heap_alloc(heap, size);
		close(heap);
	} else {
		fprintf(stderr, "[surf] dma_heap unavailable (%s); "
			"using memfd backing (not GPU-importable)\n",
			strerror(errno));
		bfd = memfd_alloc(size);
	}
	if (bfd < 0) {
		perror("[surf] alloc backing");
		return -1;
	}
	map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, bfd, 0);
	if (map == MAP_FAILED) {
		perror("[surf] mmap");
		close(bfd);
		return -1;
	}

	s = &t->s[t->n++];
	s->id = id;
	s->bfd = bfd;
	s->bmap = map;
	s->bsize = size;
	s->sw = width;
	s->sh = height;
	s->fourcc = fourcc;
	s->decoded = 0;
	s->queued = 0;
	s->write_started = 0;
	s->fence_token = 0;
	s->generation = 0;
	s->owner = NULL;
	return 0;
}

void
iris_surfs_free(struct iris_surfs *t, VASurfaceID id)
{
	int i;

	if (!t)
		return;
	for (i = 0; i < t->n; i++) {
		struct iris_surface *s = &t->s[i];

		if (s->id != id)
			continue;
		if (s->owner && s->owner->dec_open &&
		    (s->write_started || s->fence_token))
			surface_finish_write(&s->owner->dec, s);
		munmap(s->bmap, s->bsize);
		close(s->bfd);
		t->s[i] = t->s[t->n - 1];
		t->n--;
		return;
	}
}

/* Actual NV12 layout of @s: what its owning engine negotiated for CAPTURE,
 * or the aligned coded size before a session exists.  Export/derive must
 * describe buffers with these values, never with assumptions, or clients
 * read garbled rows. */
static void
surface_layout(const struct iris_surface *s, unsigned int *pitch,
	       unsigned int *width, unsigned int *height)
{
	unsigned int p = surface_pitch(s->sw, s->fourcc);
	unsigned int w = s->sw;
	unsigned int h = ALIGN_TO(s->sh, 32);

	if (s->owner && s->owner->dec_open &&
	    s->owner->dec.cap_fmt.fmt.pix_mp.width &&
	    s->owner->dec.cap_fmt.fmt.pix_mp.plane_fmt[0].bytesperline) {
		p = s->owner->dec.cap_fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
		w = s->owner->dec.cap_fmt.fmt.pix_mp.width;
		h = s->owner->dec.cap_fmt.fmt.pix_mp.height;
	}
	*pitch = p;
	*width = w;
	*height = h;
}

static int
iris_decode_surface_ready(struct iris_decode_ctx *ctx, VASurfaceID id);

int
iris_surfs_sync(struct iris_surfs *t, VASurfaceID id)
{
	struct iris_surface *s = surfs_find(t, id);

	/* Never-queued and already-decoded surfaces succeed without
	 * draining anything: Chrome syncs freshly allocated pool surfaces
	 * before exporting them and must not get spurious timeouts. */
	if (!s)
		return -EINVAL;
	if (!s->queued || s->decoded)
		return 0;
	/* Drain whichever engine queued this picture; with one engine per
	 * context that is exactly the context still holding the stream. */
	if (s->owner) {
		int r = iris_decode_sync(s->owner, id);

		DBG("[surfs_sync] id=%u -> engine sync r=%d\n", id, r);
		return r;
	}
	return 0;
}

int
iris_surfs_ready(struct iris_surfs *t, VASurfaceID id)
{
	struct iris_surface *s = surfs_find(t, id);

	return s ? s->decoded : 0;
}

int
iris_surfs_valid(struct iris_surfs *t, VASurfaceID id)
{
	return surfs_find(t, id) != NULL;
}

int
iris_surfs_export(struct iris_surfs *t, VASurfaceID id, int *fd,
		  unsigned int *pitch, unsigned int *size,
		  unsigned int *width, unsigned int *height,
		  unsigned int *fourcc)
{
	struct iris_surface *s = surfs_find(t, id);
	int exported_fd;
	unsigned int p, w, h;

	if (!s)
		return -1;
	surface_layout(s, &p, &w, &h);
	/* vaExportSurfaceHandle transfers ownership of every returned object
	 * fd to the caller.  Keep the driver's backing fd private: Chrome
	 * closes the exported fd after importing it, and returning s->bfd
	 * directly caused a double-close/FD-reuse crash in the GPU process. */
	exported_fd = fcntl(s->bfd, F_DUPFD_CLOEXEC, 0);
	if (exported_fd < 0)
		return -1;
	*fd = exported_fd;
	*pitch = p;
	*size = s->bsize;
	*width = w;
	*height = h;
	*fourcc = s->fourcc;
	return 0;
}

int
iris_surfs_buffer(struct iris_surfs *t, VASurfaceID id, void **mem,
		  unsigned int *pitch, unsigned int *size,
		  unsigned int *width, unsigned int *height,
		  unsigned int *fourcc)
{
	struct iris_surface *s = surfs_find(t, id);
	unsigned int p, w, h;

	if (!s)
		return -1;
	surface_layout(s, &p, &w, &h);
	*mem = s->bmap;
	*pitch = p;
	*size = s->bsize;
	*width = w;
	*height = h;
	*fourcc = s->fourcc;
	return 0;
}

static int
ensure_decoder(struct iris_decode_ctx *ctx)
{
	int ret;

	if (ctx->dec_open) {
		/* After an EOS flush the firmware is done; a client that keeps
		 * decoding (Chrome flush/reset, looped playback) needs a fresh
		 * session. */
		if (ctx->eos_sent) {
			finish_pending_writes(ctx);
			v4l2_dec_close(&ctx->dec);
			ctx->dec_open = 0;
			reset_stream_state(ctx);
		} else {
			return 0;
		}
	}
	ret = v4l2_dec_open(&ctx->dec, "/dev/video0", ctx->width,
			    ctx->height, ctx->out_pixfmt, ctx->cap_pixfmt);
	if (ret)
		return ret;
	if (ctx->direct_capture) {
		int fds[IRIS_MAX_SURFACES];
		size_t sizes[IRIS_MAX_SURFACES];
		unsigned int i;

		direct_collect_surfaces(ctx);
		if (ctx->direct_count < ctx->direct_requested_count) {
			v4l2_dec_close(&ctx->dec);
			return -EINVAL;
		}
		ctx->direct_count = ctx->direct_requested_count;
		for (i = 0; i < ctx->direct_count; i++) {
			fds[i] = ctx->direct[i].fd;
			sizes[i] = ctx->direct[i].size;
		}
		ret = v4l2_dec_set_capture_dmabufs(&ctx->dec, fds, sizes,
						     ctx->direct_count);
		if (ret) {
			v4l2_dec_close(&ctx->dec);
			return ret;
		}
	}
	ctx->dec_open = 1;
	return 0;
}

static int
direct_surface_index(struct iris_decode_ctx *ctx, VASurfaceID id)
{
	unsigned int i;

	for (i = 0; i < ctx->direct_count; i++)
		if (ctx->direct[i].id == id)
			return (int)i;
	return -1;
}

static void
hevc_pending_add(struct iris_decode_ctx *ctx, int32_t poc, VASurfaceID target,
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
hevc_pending_take(struct iris_decode_ctx *ctx, VASurfaceID *target,
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


/* Assign one dequeued frame to its surface.  H.264/VP9 use the timestamp
 * propagated by the firmware.  Iris does not preserve usable HEVC timestamps,
 * so HEVC matches display-order CAPTURE frames against pending picture POCs.
 * Returns the surface id, or -1 if unknown. */
static int
assign_frame(struct iris_decode_ctx *ctx, const struct v4l2_dec_frame *frame)
{
	VASurfaceID id;
	uint64_t generation;
	struct iris_surface *s;

	if (ctx->out_pixfmt == V4L2_PIX_FMT_HEVC) {
		if (hevc_pending_take(ctx, &id, &generation)) {
			v4l2_dec_qcap_idx(&ctx->dec, frame->index);
			return -EIO;
		}
	} else {
		uint64_t frame_seq, slot;

		if (frame->timestamp < 1000000000ULL) {
			v4l2_dec_qcap_idx(&ctx->dec, frame->index);
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
			v4l2_dec_qcap_idx(&ctx->dec, frame->index);
			return 0;
		}
		id = ctx->target_ring[slot].target;
		generation = ctx->target_ring[slot].generation;
		ctx->target_ring[slot].used = 0;
	}
	s = find_surface(ctx, id);

	if (!s) {
		DBG("[assign] NO SURFACE id=%u\n", id);
		v4l2_dec_qcap_idx(&ctx->dec, frame->index);
		return 0;
	}
	/* A VA client may recycle a render target as soon as it drops the old
	 * output frame, while legacy Iris firmware can still have that picture
	 * queued internally (up to its display/decode hold depth).  Do not let
	 * such a late CAPTURE buffer overwrite the newer picture already mapped
	 * to the same stable backing, nor signal the newer picture's fence. */
	if (s->generation != generation) {
		DBG("[assign] STALE id=%u generation=%llu current=%llu\n", id,
		    (unsigned long long)generation,
		    (unsigned long long)s->generation);
		v4l2_dec_qcap_idx(&ctx->dec, frame->index);
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
		surface_finish_write(&ctx->dec, s);
		s->decoded = 1;
		s->queued = 1;
		s->owner = ctx;
		ctx->stats_direct_frames++;
		return id;
	}
	/* Copy the decoded frame into the surface's stable DMA-heap backing so
	 * buffers exported before decoding stay valid, then recycle the
	 * firmware buffer. */
	if (frame->bytesused > s->bsize) {
		fprintf(stderr,
			"[copy] WARNING frame %u bytes > backing %u; "
			"dropping surface %u\n",
			frame->bytesused, s->bsize, id);
		surface_finish_write(&ctx->dec, s);
		v4l2_dec_qcap_idx(&ctx->dec, frame->index);
		return -1;
	} else if (frame->bytesused) {
		int copy_ret = surface_copy(ctx, s, frame->mem,
					    frame->bytesused);

		if (copy_ret < 0) {
			fprintf(stderr,
				"[copy] DMA-BUF sync/copy failed for surface %u: %s\n",
				id, strerror(-copy_ret));
			v4l2_dec_qcap_idx(&ctx->dec, frame->index);
			return -1;
		}
	} else {
		surface_finish_write(&ctx->dec, s);
	}
	{
		int ret = v4l2_dec_qcap_idx(&ctx->dec, frame->index);

		if (ret)
			return ret;
	}
	s->decoded = 1;
	s->queued = 1;
	s->owner = ctx;
	return id;
}

static int drain_available(struct iris_decode_ctx *ctx);

/* Wait for one already-submitted render target without flushing the stream.
 * The V4L2 session requests decode-order output, so firmware can complete the
 * current target without needing another picture to be queued.
 * This is used before vaEndPicture returns: the ANGLE/GL import path on legacy
 * Adreno does not reliably wait for reservation fences attached after the
 * DMA-BUF was imported, and can otherwise sample that surface's old pixels. */
static int
wait_surface_ready(struct iris_decode_ctx *ctx, VASurfaceID id, int deadline)
{
	while (deadline-- > 0) {
		struct iris_surface *s;
		struct v4l2_dec_frame frame;
		int changed, ret;

		ret = drain_available(ctx);
		if (ret)
			return ret;
		s = find_surface(ctx, id);
		if (s && s->decoded)
			return 0;

		ret = v4l2_dec_poll_cap(&ctx->dec, 20);
		if (ret < 0)
			return ret;
		if (!ret)
			continue;
		ret = v4l2_dec_handle_events(&ctx->dec, &changed);
		if (ret)
			return ret;
		while (v4l2_dec_dqout(&ctx->dec) == 0)
			;
		ret = v4l2_dec_dqcap(&ctx->dec, &frame);
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
int
iris_decode_flush(struct iris_decode_ctx *ctx)
{
	struct v4l2_dec_frame frame;
	int ret, deadline = 100;

	if (!ctx->dec_open || !ctx->dec_started || ctx->eos_sent)
		return ctx->fatal_error;

	ret = v4l2_dec_flush(&ctx->dec);
	if (ret)
		return ret;
	ctx->eos_sent = 1;

	/* Drain decoded pictures until the empty V4L2_BUF_FLAG_LAST marker is
	 * dequeued.  Pictures preceding that marker are assigned normally. */
	while (deadline-- > 0) {
		int changed;

		ret = v4l2_dec_poll_cap(&ctx->dec, 20);
		if (ret < 0) {
			ctx->fatal_error = ret;
			return ret;
		}
		if (!ret)
			continue;
		ret = v4l2_dec_handle_events(&ctx->dec, &changed);
		if (ret)
			return ret;
		while (v4l2_dec_dqout(&ctx->dec) == 0)
			;
		ret = v4l2_dec_dqcap(&ctx->dec, &frame);
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
	return ctx->dec.eos ? 0 : -ETIMEDOUT;
}

/* Non-blocking drain of whatever frames are ready. */
static int
drain_available(struct iris_decode_ctx *ctx)
{
	if (ctx->fatal_error)
		return ctx->fatal_error;
	if (ctx->eos_sent)
		return 0;

	for (;;) {
		struct v4l2_dec_frame frame;
		int changed, ret;

		ret = v4l2_dec_poll(&ctx->dec, 0);
		if (ret < 0) {
			ctx->fatal_error = ret;
			return ret;
		}
		if (!ret)
			break;
		ret = v4l2_dec_handle_events(&ctx->dec, &changed);
		if (ret)
			return ret;
		while (v4l2_dec_dqout(&ctx->dec) == 0)
			;
		ret = v4l2_dec_dqcap(&ctx->dec, &frame);
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
	}
	return 0;
}

int
iris_decode_begin(struct iris_decode_ctx *ctx, VASurfaceID target)
{
	struct iris_surface *s = find_surface(ctx, target);
	int direct_index = ctx->direct_capture ?
		direct_surface_index(ctx, target) : -1;

	if (ctx->direct_error)
		return ctx->direct_error;
	if (ctx->fatal_error)
		return ctx->fatal_error;
	if (!s)
		return -EINVAL;
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
	    !ctx->dec.cap_queued[direct_index]) {
		int ret = v4l2_dec_qcap_idx(&ctx->dec, direct_index);

		if (ret)
			return ret;
	}

	DBG("[begin] target=%u decoded=%d queued=%d generation=%llu\n",
	    target, s ? s->decoded : -1, s ? s->queued : -1,
	    (unsigned long long)(s ? s->generation : 0));
	ctx->current_target = target;
	/* VA clients reuse render targets.  A surface that held an earlier
	 * picture must become pending again, otherwise vaSyncSurface can return
	 * the stale backing before the newly decoded picture is copied into it. */
	s->generation++;
	ctx->current_generation = s->generation;
	s->decoded = 0;
	s->queued = 0;
	ctx->have_pic = 0;
	ctx->have_hevc_pic = 0;
	ctx->slice_len = 0;
	ctx->refs_l0 = 0;
	ctx->refs_l1 = 0;
	ctx->h264_slice_count = 0;
	ctx->hevc_slice_count = 0;
	ctx->hevc_slice_next = 0;
	ctx->hevc_pps_id = -1;
	ctx->hevc_rewritten = 0;
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
	if (ctx->h264_slice_count >= ARRAY_SIZE(ctx->h264_slices))
		return -1;
	memcpy(&ctx->h264_slices[ctx->h264_slice_count].param, sp,
	       sizeof(*sp));
	ctx->h264_slice_count++;
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

int
iris_decode_h264_iq_matrix(struct iris_decode_ctx *ctx,
			   const VAIQMatrixBufferH264 *iq)
{
	if (!iq)
		return -EINVAL;
	memcpy(&ctx->h264_iq, iq, sizeof(*iq));
	ctx->have_h264_iq = 1;
	return 0;
}

int
iris_decode_hevc_picture(struct iris_decode_ctx *ctx,
			 const VAPictureParameterBufferHEVC *pic)
{
	memcpy(&ctx->hevc_pic, pic, sizeof(*pic));
	ctx->have_hevc_pic = 1;
	return 0;
}

int
iris_decode_hevc_iq_matrix(struct iris_decode_ctx *ctx,
			   const VAIQMatrixBufferHEVC *iq)
{
	if (!iq)
		return -EINVAL;
	memcpy(&ctx->hevc_iq, iq, sizeof(*iq));
	ctx->have_hevc_iq = 1;
	return 0;
}

int
iris_decode_hevc_slice_params(struct iris_decode_ctx *ctx,
			      const VASliceParameterBufferHEVC *sp)
{
	if (sp->num_ref_idx_l0_active_minus1 > ctx->refs_l0)
		ctx->refs_l0 = sp->num_ref_idx_l0_active_minus1;
	if (sp->num_ref_idx_l1_active_minus1 > ctx->refs_l1)
		ctx->refs_l1 = sp->num_ref_idx_l1_active_minus1;
	/* VA-API may submit one parameter buffer containing many slice elements.
	 * Keep every range; retaining only the last element corrupts multi-slice
	 * 4K/8K pictures. */
	if (ctx->hevc_slice_count >= 128)
		return -1;
	memcpy(&ctx->hevc_slices[ctx->hevc_slice_count].param, sp, sizeof(*sp));
	ctx->hevc_slice_count++;
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

/* VA exposes the effective reference counts for each slice.  Those values
 * include slice-header overrides and therefore must not be copied into the
 * PPS on every picture.  Find slices that actually use the PPS defaults and
 * learn the stable values from their VA parameters. */
static int
h264_update_pps_defaults(struct iris_decode_ctx *ctx)
{
	size_t i = 0;
	unsigned int slice = 0;

	while (i + 3 < ctx->slice_len) {
		int sc = has_start_code(ctx->slice_data + i,
					ctx->slice_len - i);
		size_t start, end;
		int mask;
		const VASliceParameterBufferH264 *sp;

		if (!sc) {
			i++;
			continue;
		}
		start = i + sc;
		end = start;
		while (end < ctx->slice_len &&
		       !has_start_code(ctx->slice_data + end,
				       ctx->slice_len - end))
			end++;
		if (end <= start || (ctx->slice_data[start] & 0x1f) < 1 ||
		    (ctx->slice_data[start] & 0x1f) > 5) {
			i = end;
			continue;
		}
		if (slice >= ctx->h264_slice_count)
			return -1;
		sp = &ctx->h264_slices[slice++].param;
		mask = h264_slice_default_ref_mask(ctx->slice_data + start,
						  end - start, &ctx->pic);
		if (mask < 0)
			return -1;
		if (mask & 1)
			ctx->h264_pps_refs_l0 =
				sp->num_ref_idx_l0_active_minus1;
		if (mask & 2)
			ctx->h264_pps_refs_l1 =
				sp->num_ref_idx_l1_active_minus1;
		i = end;
	}
	return slice == ctx->h264_slice_count ? 0 : -1;
}

static int
hevc_cache_raw_parameter_sets(struct iris_decode_ctx *ctx)
{
	size_t i = 0;
	int found = 0;

	while (i + 3 < ctx->slice_len) {
		int sc = has_start_code(ctx->slice_data + i, ctx->slice_len - i);
		size_t start, end;
		uint8_t type;

		if (!sc) {
			i++;
			continue;
		}
		start = i + sc;
		end = start;
		while (end < ctx->slice_len &&
		       !has_start_code(ctx->slice_data + end,
				       ctx->slice_len - end))
			end++;
		if (end <= start + 1) {
			i = end;
			continue;
		}
		type = (ctx->slice_data[start] >> 1) & 0x3f;
		if (type == 32 && end - start <= sizeof(ctx->raw_hevc_vps)) {
			memcpy(ctx->raw_hevc_vps, ctx->slice_data + start, end - start);
			ctx->raw_hevc_vps_len = end - start;
			found |= 1;
		} else if (type == 33 && end - start <= sizeof(ctx->raw_hevc_sps)) {
			memcpy(ctx->raw_hevc_sps, ctx->slice_data + start, end - start);
			ctx->raw_hevc_sps_len = end - start;
			found |= 2;
		} else if (type == 34 && end - start <= sizeof(ctx->raw_hevc_pps)) {
			memcpy(ctx->raw_hevc_pps, ctx->slice_data + start, end - start);
			ctx->raw_hevc_pps_len = end - start;
			found |= 4;
		}
		i = end;
	}
	return found;
}

int
iris_decode_slice(struct iris_decode_ctx *ctx, const void *data, size_t len)
{
	const uint8_t *p = data;
	size_t need = len + 4;

	if (ctx->out_pixfmt == V4L2_PIX_FMT_HEVC &&
	    ctx->hevc_slice_next < ctx->hevc_slice_count) {
		unsigned int use = 0;
		unsigned int i;
		size_t data_len = len;
		unsigned int remaining = ctx->hevc_slice_count - ctx->hevc_slice_next;

		/* A single VA slice-data buffer normally contains all ranges.  If a
		 * client uses one data buffer per slice, consume one range at a time. */
		if (remaining > 1) {
			int all_fit = 1;
			for (i = 0; i < remaining; i++) {
				const VASliceParameterBufferHEVC *sp =
					&ctx->hevc_slices[ctx->hevc_slice_next + i].param;

				if ((uint64_t)sp->slice_data_offset +
				    sp->slice_data_size > data_len) {
					all_fit = 0;
					break;
				}
			}
			if (all_fit)
				use = remaining;
		}
		if (!use)
			use = 1;

		for (i = 0; i < use; i++) {
			const VASliceParameterBufferHEVC *sp =
				&ctx->hevc_slices[ctx->hevc_slice_next + i].param;
			uint32_t off = sp->slice_data_offset;
			uint32_t size = sp->slice_data_size;
			uint8_t *rewritten;
			size_t rewritten_cap, nal_len;
			unsigned int pps_id;
			int sc, rewritten_len;
			uint64_t rewrite_start;

			if ((uint64_t)off + size > data_len || !ctx->have_hevc_pic)
				return -1;
			p = (const uint8_t *)data + off;
			sc = has_start_code(p, size);
			nal_len = size - sc;
			if (nal_len > (SIZE_MAX - 1024) / 2)
				return -1;
			rewritten_cap = nal_len * 2 + 1024;
			rewrite_start = ctx->stats_enabled ? monotonic_ns() : 0;
			if (ctx->slice_len > SIZE_MAX - 4 ||
			    rewritten_cap > SIZE_MAX - ctx->slice_len - 4)
				return -1;
			need = 4 + rewritten_cap;
			if (ctx->slice_len + need > ctx->slice_cap) {
				size_t ncap = ctx->slice_cap ? ctx->slice_cap : (1 << 20);

				while (ncap < ctx->slice_len + need) {
					if (ncap > SIZE_MAX / 2)
						return -1;
					ncap *= 2;
				}
				void *n = realloc(ctx->slice_data, ncap);

				if (!n)
					return -1;
				ctx->slice_data = n;
				ctx->slice_cap = ncap;
			}
			rewritten = ctx->slice_data + ctx->slice_len + 4;
			rewritten_len = hevc_rewrite_slice(rewritten, rewritten_cap,
							 p + sc, nal_len,
							 &ctx->hevc_pic, sp,
							 &pps_id);
			if (rewritten_len < 0) {
				fprintf(stderr, "HEVC slice rewrite failed: %d\n",
					rewritten_len);
				return rewritten_len;
			}
			if (ctx->hevc_pps_id < 0)
				ctx->hevc_pps_id = (int)pps_id;
			else if (ctx->hevc_pps_id != (int)pps_id) {
				return -1;
			}
			if ((size_t)rewritten_len != nal_len ||
			    memcmp(rewritten, p + sc, nal_len))
				ctx->hevc_rewritten = 1;
			{
				static const uint8_t sc4[4] = { 0, 0, 0, 1 };
				memcpy(ctx->slice_data + ctx->slice_len, sc4, 4);
				ctx->slice_len += 4;
			}
			ctx->slice_len += rewritten_len;
			if (ctx->stats_enabled) {
				ctx->stats_rewrite_ns += monotonic_ns() - rewrite_start;
				ctx->stats_rewrite_bytes += nal_len;
				ctx->stats_rewrites++;
			}
		}
		ctx->hevc_slice_next += use;
		return 0;
	}

	if (ctx->slice_len + need > ctx->slice_cap) {
		size_t ncap = ctx->slice_cap ? ctx->slice_cap * 2 :
					      (len + (1 << 20));
		void *n = realloc(ctx->slice_data, ncap);

		if (!n)
			return -1;
		ctx->slice_data = n;
		ctx->slice_cap = ncap;
	}

	if (!has_start_code(p, len) && ctx->out_pixfmt != V4L2_PIX_FMT_VP9) {
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
	int rv;
	uint64_t end_start = ctx->stats_enabled ? monotonic_ns() : 0;

	DBG("[end] target=%u slice_len=%zu refs=%d/%d started=%d\n",
	    ctx->current_target, ctx->slice_len, ctx->refs_l0, ctx->refs_l1,
	    ctx->dec_started);
	size_t au_len = 0, au_cap;
	int n, ret;
	static const uint8_t sc4[4] = { 0, 0, 0, 1 };

	if (!ctx->have_pic && ctx->out_pixfmt != V4L2_PIX_FMT_VP9 &&
	    !ctx->have_hevc_pic)
		return -1;
	if (ctx->out_pixfmt == V4L2_PIX_FMT_HEVC &&
	    ctx->hevc_slice_next != ctx->hevc_slice_count)
		return -1;

	/* 16 MiB of stack would overflow the caller's stack; use the heap. */
	au_cap = ctx->slice_len + sizeof(ctx->last_sps) +
		 sizeof(ctx->last_pps) + 16;
	if (ctx->out_pixfmt == V4L2_PIX_FMT_HEVC)
		au_cap += sizeof(ctx->raw_hevc_vps) + sizeof(ctx->raw_hevc_sps) +
			  sizeof(ctx->raw_hevc_pps) + sizeof(ctx->last_hevc_sps) +
			  16;
	if (ctx->au_cap < au_cap) {
		void *n = realloc(ctx->au_data, au_cap);

		if (!n)
			return -1;
		ctx->au_data = n;
		ctx->au_cap = au_cap;
	}
	au = ctx->au_data;
	ret = ensure_decoder(ctx);
	if (ret)
		return ret;

	/* Assemble the access unit to feed the stateful firmware.
	 *
	 * H.264: VA clients send picture/slice parameter buffers, so re-serialize
	 * SPS/PPS.  HEVC prefers original VPS/SPS/PPS NALs when available and
	 * only uses the serializer as a compatibility fallback.
	 *
	 * VP9: each frame is self-contained (its own uncompressed+compressed
	 * header); the slice data is the whole frame, feed it verbatim. */
	if (ctx->out_pixfmt == V4L2_PIX_FMT_VP9) {
		au_len = ctx->slice_len;
		memcpy(au, ctx->slice_data, au_len);
	} else if (ctx->out_pixfmt == V4L2_PIX_FMT_HEVC) {
		int raw_in_au = ctx->hevc_rewritten ? 0 :
			hevc_cache_raw_parameter_sets(ctx);
		/* If the client provided complete parameter NALs, pass the original
		 * Annex-B access unit through unchanged.  This is the native input
		 * contract of the stateful Iris V4L2 decoder. */
		if (raw_in_au == 7 && !ctx->hevc_rewritten) {
			au_len = ctx->slice_len;
			memcpy(au, ctx->slice_data, au_len);
			goto hevc_au_ready;
		}
		if (!ctx->hevc_rewritten && ctx->raw_hevc_vps_len &&
		    ctx->raw_hevc_sps_len && ctx->raw_hevc_pps_len) {
			memcpy(au + au_len, sc4, 4);
			memcpy(au + au_len + 4, ctx->raw_hevc_vps,
			       ctx->raw_hevc_vps_len);
			au_len += 4 + ctx->raw_hevc_vps_len;
			memcpy(au + au_len, sc4, 4);
			memcpy(au + au_len + 4, ctx->raw_hevc_sps,
			       ctx->raw_hevc_sps_len);
			au_len += 4 + ctx->raw_hevc_sps_len;
			memcpy(au + au_len, sc4, 4);
			memcpy(au + au_len + 4, ctx->raw_hevc_pps,
			       ctx->raw_hevc_pps_len);
			au_len += 4 + ctx->raw_hevc_pps_len;
			memcpy(au + au_len, ctx->slice_data, ctx->slice_len);
			au_len += ctx->slice_len;
			goto hevc_au_ready;
		}
		/* Re-serialize VPS/SPS/PPS from the picture params and prepend
		 * only when they change (per-picture repetition resets the
		 * firmware DPB).  ffmpeg/Chrome send bare slice NALs. */
		n = hevc_build_vps(au + 4, au_cap - 4 - ctx->slice_len,
				   &ctx->hevc_pic);
		if (n <= 0) {
			return -1;
		}
		if (n > 0 && (n != ctx->last_hevc_vps_len ||
			      memcmp(au + 4, ctx->last_hevc_vps, n) != 0)) {
			memcpy(au, sc4, 4);
			memcpy(ctx->last_hevc_vps, au + 4, n);
			ctx->last_hevc_vps_len = n;
			au_len = 4 + n;
		}
		n = hevc_build_sps(au + au_len + 4, au_cap - au_len - 4 -
				   ctx->slice_len, &ctx->hevc_pic,
				   ctx->have_hevc_iq ? &ctx->hevc_iq : NULL);
		if (n <= 0) {
			return -1;
		}
		if (n > 0 && (n != ctx->last_hevc_sps_len ||
			      memcmp(au + au_len + 4, ctx->last_hevc_sps, n) != 0)) {
			memcpy(au + au_len, sc4, 4);
			memcpy(ctx->last_hevc_sps, au + au_len + 4, n);
			ctx->last_hevc_sps_len = n;
			au_len += 4 + n;
		}
		n = hevc_build_pps_id(au + au_len + 4, au_cap - au_len - 4 -
				      ctx->slice_len, &ctx->hevc_pic,
				      ctx->hevc_pps_id < 0 ? 0 :
				      (unsigned int)ctx->hevc_pps_id);
		if (n <= 0) {
			return -1;
		}
		if (n > 0 && (n != ctx->last_hevc_pps_len ||
			      memcmp(au + au_len + 4, ctx->last_hevc_pps, n) != 0)) {
			memcpy(au + au_len, sc4, 4);
			memcpy(ctx->last_hevc_pps, au + au_len + 4, n);
			ctx->last_hevc_pps_len = n;
			au_len += 4 + n;
		}
		if (au_len + ctx->slice_len > au_cap) {
			return -1;
		}
		memcpy(au + au_len, ctx->slice_data, ctx->slice_len);
		au_len += ctx->slice_len;
	hevc_au_ready:
		;
	} else {
	/* An IDR slice does not use the PPS reference defaults, so VA reports
	 * zero for both lists.  Starting the stream with those zeros and later
	 * redefining pps_id 0 when the first P/B slice arrives crashes legacy
	 * Venus firmware.  Seed the conventional decoder default (up to three
	 * active L0 references, bounded by max_num_ref_frames); a later slice
	 * that actually uses the defaults supplies the exact values. */
	if (ctx->h264_pps_refs_l0 < 0)
		ctx->h264_pps_refs_l0 = ctx->pic.num_ref_frames ?
			(ctx->pic.num_ref_frames > 3 ? 2 :
			 ctx->pic.num_ref_frames - 1) : 0;
	if (ctx->h264_pps_refs_l1 < 0)
		ctx->h264_pps_refs_l1 = 0;
	if (h264_update_pps_defaults(ctx))
		return -1;
	/* Only re-emit SPS/PPS when they change; a per-picture repetition
	 * resets the firmware DPB and breaks P-frame references. */
	n = h264_build_sps(au + 4, au_cap - 4 - ctx->slice_len, &ctx->pic,
			   ctx->profile, ctx->width, ctx->height);
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
			   ctx->slice_len, &ctx->pic,
			   ctx->have_h264_iq ? &ctx->h264_iq : NULL,
			   ctx->h264_pps_refs_l0 < 0 ? 0 :
			   ctx->h264_pps_refs_l0,
			   ctx->h264_pps_refs_l1 < 0 ? 0 :
			   ctx->h264_pps_refs_l1);
	if (n <= 0) {
		return -1;
	}
	if (n != ctx->last_pps_len ||
	    memcmp(au + au_len + 4, ctx->last_pps, n) != 0) {
		memcpy(au + au_len, sc4, 4);
		memcpy(ctx->last_pps, au + au_len + 4, n);
		ctx->last_pps_len = n;
		au_len += 4 + n;
	}

	if (au_len + ctx->slice_len > au_cap) {
		return -1;
	}
	memcpy(au + au_len, ctx->slice_data, ctx->slice_len);
	au_len += ctx->slice_len;
	}

	/* Each V4L2 OUTPUT buffer must contain exactly one access unit.  In
	 * particular, do not append an AUD: it starts an empty next access unit
	 * and crashes legacy Iris firmware on otherwise valid streams. */

	/* Opt-in bitstream capture for validating the VA-to-stateful translation
	 * with an independent software decoder. */
	if (ctx->out_pixfmt == V4L2_PIX_FMT_HEVC) {
		const char *dump_path = getenv("IRIS_HEVC_DUMP");

		if (dump_path && *dump_path) {
			FILE *dump = fopen(dump_path, "ab");

			if (dump) {
				fwrite(au, 1, au_len, dump);
				fclose(dump);
			}
		}
	} else if (ctx->out_pixfmt == V4L2_PIX_FMT_H264) {
		const char *dump_path = getenv("IRIS_H264_DUMP");

		if (dump_path && *dump_path) {
			FILE *dump = fopen(dump_path, "ab");

			if (dump) {
				fwrite(au, 1, au_len, dump);
				fclose(dump);
			}
		}
	}

	{
		uint64_t ts = (ctx->seq + 1000) * 1000000000ULL;

		ctx->last_target = ctx->current_target;
		{
			struct iris_surface *qt = find_surface(ctx,
							       ctx->current_target);

			if (qt) {
				uint64_t token = ctx->seq + 1;

				qt->queued = 1;
				qt->owner = ctx;
				if (ctx->direct_capture)
					surface_begin_device_write(&ctx->dec, qt, token);
				else
					surface_begin_write(&ctx->dec, qt, token);
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

		if (!ctx->dec_started) {
			/* Mirror FFmpeg: queue the first access unit before
			 * STREAMON, otherwise the firmware rejects the CAPTURE
			 * setup (0x1004). */
			ret = v4l2_dec_feed(&ctx->dec, au, au_len, ts);
			if (ret) {
				finish_pending_writes(ctx);
				return ret;
			}
			ret = v4l2_dec_start(&ctx->dec);
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
				while (v4l2_dec_dqout(&ctx->dec) == 0)
					;
				ret = v4l2_dec_feed(&ctx->dec, au, au_len, ts);
				if (ret != -EAGAIN)
					break;
				ret = drain_available(ctx);
				if (ret)
					break;
				v4l2_dec_poll(&ctx->dec, 50);
			}
			if (ret) {
				finish_pending_writes(ctx);
				return ret;
			}
		}
		if (ctx->out_pixfmt == V4L2_PIX_FMT_HEVC)
			hevc_pending_add(ctx,
					 ctx->hevc_pic.CurrPic.pic_order_cnt,
					 ctx->current_target,
					 ctx->current_generation);
		ctx->seq++;
	}
	/* The stateful firmware holds each frame until the next access unit
	 * arrives.  Draining right after this feed makes the *previous*
	 * picture's frame available so a client that syncs one picture at a
	 * time (ffmpeg/Chrome) does not deadlock. */
	ret = drain_available(ctx);
	if (ret)
		return ret;

	/* Chrome sends a decoded frame to ANGLE immediately after vaEndPicture.
	 * Its GL DMA-BUF import can sample the previous contents even though a
	 * new reservation fence was attached.  H.264 is emitted in decode order
	 * and each AU ends in AUD, so wait here until the target backing contains
	 * this picture.  This applies hardware backpressure and turns overload
	 * into ordinary frame dropping instead of visible backward frames. */
	if (ctx->out_pixfmt == V4L2_PIX_FMT_H264) {
		ret = wait_surface_ready(ctx, ctx->current_target, 100);
		if (ret) {
			DBG("[end] target=%u readiness wait failed: %d\n",
			    ctx->current_target, ret);
			return ret;
		}
	}

	ctx->slice_len = 0;
	ctx->have_pic = 0;
	rv = 0;
	if (ctx->stats_enabled) {
		ctx->stats_end_ns += monotonic_ns() - end_start;
		ctx->stats_ends++;
	}
	DBG("[end] done rv=%d\n", rv);
	return rv;
}

int
iris_decode_sync(struct iris_decode_ctx *ctx, VASurfaceID id)
{
	struct iris_surface *s = find_surface(ctx, id);
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

	if (iris_decode_surface_ready(ctx, id))
		return 0;
	if (ctx->fatal_error)
		return ctx->fatal_error;

	/* The stateful firmware holds the last queued picture until an EOS
	 * marker arrives.  If the client is syncing that final picture, feed
	 * EOS to force it out instead of spinning until the timeout. */
	if (id == ctx->last_target && !ctx->eos_sent) {
		DBG("[sync] last_target=%u: flushing\n", id);
		if (iris_decode_flush(ctx) == 0 &&
		    iris_decode_surface_ready(ctx, id))
			return 0;
	}

	while (deadline-- > 0) {
		struct v4l2_dec_frame frame;
		int changed, ret;

		/* Wait for real CAPTURE progress; POLLOUT would wake us
		 * instantly and burn the deadline before any frame is done. */
		ret = v4l2_dec_poll_cap(&ctx->dec, 20);
		if (ret < 0) {
			ctx->fatal_error = ret;
			return ret;
		}
		if (!ret)
			continue;
		(void)changed;
		ret = v4l2_dec_handle_events(&ctx->dec, &changed);
		if (ret)
			return ret;
		while (v4l2_dec_dqout(&ctx->dec) == 0)
			;
		ret = v4l2_dec_dqcap(&ctx->dec, &frame);
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
		if (iris_decode_surface_ready(ctx, id))
			return 0;
	}
	return -ETIMEDOUT;
}

static int
iris_decode_surface_ready(struct iris_decode_ctx *ctx, VASurfaceID id)
{
	if (drain_available(ctx))
		return 0;
	{
		struct iris_surface *s = find_surface(ctx, id);

		return s ? s->decoded : 0;
	}
}
