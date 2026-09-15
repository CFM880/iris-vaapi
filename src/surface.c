// SPDX-License-Identifier: GPL-2.0-or-later
/* Display-level surface registry: stable exportable backings, stream-epoch
 * invalidation and the metadata exported through vaDeriveImage/vaExportSurface.
 *
 * Surfaces belong to the VA display, not to a decode context, so they outlive
 * the engine that decoded into them and may be recycled across seeks. */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>

#include "decode_internal.h"

/* Unique identity for every backing and Vulkan capture key. */
uint64_t g_buffer_serial;

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC	0x0001U
#endif

/* Diagnostic frame counter for VPU_FRAME_STAMP: every decoded frame written
 * into a backing gets a unique serial painted as a barcode so the seek
 * harness can prove which driver frame the compositor is displaying. */
static uint64_t g_frame_stamp;

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
	int fd = (int)syscall(SYS_memfd_create, "vpu-surface", MFD_CLOEXEC);

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

static void
surface_fill_black(int fd, void *map, unsigned int pitch,
		   unsigned int height, unsigned int fourcc)
{
	size_t luma_size = (size_t)pitch * ALIGN_TO(height, 32);
	size_t total_size = luma_size * 3 / 2;
	int sync_started;

	sync_started = dma_buf_cpu_sync(fd,
		DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE) == 0;
	if (fourcc == VPU_PIXEL_FORMAT_P010) {
		uint16_t *pixels = map;
		size_t i;

		/* P010 stores limited-range 10-bit components in the high bits. */
		for (i = 0; i < luma_size / sizeof(*pixels); i++)
			pixels[i] = 64U << 6;
		for (; i < total_size / sizeof(*pixels); i++)
			pixels[i] = 512U << 6;
	} else {
		/* Limited-range NV12 black: Y=16, neutral interleaved UV=128. */
		memset(map, 16, luma_size);
		memset((uint8_t *)map + luma_size, 128, total_size - luma_size);
	}
	if (sync_started)
		(void)dma_buf_cpu_sync(fd,
			DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
}

unsigned int
surface_pitch(unsigned int width, unsigned int fourcc)
{
	unsigned int bytes = fourcc == VPU_PIXEL_FORMAT_P010 ? 2 : 1;
	unsigned int alignment = fourcc == VPU_PIXEL_FORMAT_P010 ? 256 : 128;

	return ALIGN_TO(width * bytes, alignment);
}

int
surface_finish_write(struct vpu_platform_session *session, struct vpu_surface *s)
{
	int ret = 0;

	if (s->write_started &&
	    dma_buf_cpu_sync(s->bfd,
			     DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE) < 0)
		ret = -errno;
	s->write_started = 0;
	if (s->fence_token) {
		int signal_ret = vpu_platform_session_signal_surface_fence(session,
							 s->fence_token);

		s->fence_token = 0;
		if (!ret && signal_ret)
			ret = signal_ret;
	}
	return ret;
}

void
surface_begin_write(struct vpu_platform_session *session, struct vpu_surface *s,
		    uint64_t token)
{
	if (s->write_started || s->fence_token)
		surface_finish_write(session, s);
	if (dma_buf_cpu_sync(s->bfd,
			     DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE) < 0)
		return;
	s->write_started = 1;
	if (!vpu_platform_session_attach_surface_fence(session, s->bfd, token)) {
		s->fence_token = token;
	} else {
		dma_buf_cpu_sync(s->bfd,
				 DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
		s->write_started = 0;
	}
}

void
surface_begin_device_write(struct vpu_platform_session *session, struct vpu_surface *s,
			   uint64_t token)
{
	if (s->write_started || s->fence_token)
		surface_finish_write(session, s);
	if (!vpu_platform_session_attach_surface_fence(session, s->bfd, token))
		s->fence_token = token;
}

static void surface_stamp(struct vpu_surface *s, uint64_t serial);

/* Record that @s now holds a valid frame decoded in the current stream epoch.
 * Every successful decode path must call this so the registry knows both that
 * the surface is presentable and which surface to repeat when a stale surface
 * is later handed to the compositor. */
void
surfaces_mark_decoded(struct vpu_decode_ctx *ctx, struct vpu_surface *s)
{
	s->decoded = 1;
	s->queued = 1;
	s->owner = ctx;
	if (ctx->surfs) {
		uint64_t serial = __atomic_add_fetch(&g_frame_stamp, 1,
						     __ATOMIC_RELAXED) + 1;

		s->epoch = ctx->surfs->epoch;
		ctx->surfs->last_frame = s->id;
		ctx->surfs->last_frame_epoch = ctx->surfs->epoch;
		surface_stamp(s, serial);
		DBG("[stamp] serial=%llu target=%u epoch=%llu\n",
		    (unsigned long long)serial, s->id,
		    (unsigned long long)ctx->surfs->epoch);
	}
}

static int
frame_stamp_enabled(void)
{
	static int value = -1;

	if (value < 0)
		value = getenv("VPU_FRAME_STAMP") != NULL;
	return value;
}

/* Diagnostic: paint the low 16 bits of @serial as a barcode across the top
 * eighth of the luma plane (16 cells, bit 1 -> Y=235, bit 0 -> Y=16) so the
 * seek harness can read back which driver frame the compositor displays. */
static void
surface_stamp(struct vpu_surface *s, uint64_t serial)
{
	unsigned int cell = s->sw / 16;
	unsigned int rows = s->sh / 8 ? s->sh / 8 : 1;
	unsigned int pitch = surface_pitch(s->sw, s->fourcc);
	int sync;

	if (!cell || !frame_stamp_enabled())
		return;
	sync = dma_buf_cpu_sync(s->bfd,
			       DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE) == 0;
	for (unsigned int b = 0; b < 16; b++) {
		uint8_t val = (serial >> b) & 1 ? 235 : 16;

		for (unsigned int y = 0; y < rows; y++)
			memset((uint8_t *)s->bmap + (size_t)y * pitch + b * cell,
			       val, cell);
	}
	if (sync)
		(void)dma_buf_cpu_sync(s->bfd,
				       DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
}

struct vpu_surface *
surfs_find(struct vpu_surfaces *t, VASurfaceID id)
{
	int i;

	if (!t)
		return NULL;
	for (i = 0; i < t->n; i++)
		if (t->s[i].id == id)
			return &t->s[i];
	return NULL;
}

struct vpu_surface *
find_surface(struct vpu_decode_ctx *ctx, VASurfaceID id)
{
	return surfs_find(ctx->surfs, id);
}

int
surface_copy(struct vpu_decode_ctx *ctx, struct vpu_surface *s,
	     const void *src, size_t size)
{
	int sync_started = s->write_started;
	int ret = 0;
	uint64_t start;
	unsigned int pitch, width, height;
	unsigned int dst_pitch = surface_pitch(s->sw, s->fourcc);
	unsigned int row_bytes = s->sw * (s->fourcc == VPU_PIXEL_FORMAT_P010 ? 2 : 1);
	int rectangular;

	vpu_platform_session_capture_layout(ctx->session, &pitch, &width, &height);
	rectangular = pitch != dst_pitch || ALIGN_TO(height, 32) != ALIGN_TO(s->sh, 32);
	if (rectangular && (!src || s->sw > width || s->sh > height ||
	    row_bytes > pitch || row_bytes > dst_pitch ||
	    (size_t)pitch * ALIGN_TO(height, 32) * 3 / 2 > size ||
	    (size_t)dst_pitch * ALIGN_TO(s->sh, 32) * 3 / 2 > s->bsize))
		return -EINVAL;
	if (!rectangular && size > s->bsize)
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

	start = ctx->stats_enabled ? vpu_monotonic_ns() : 0;
	if (rectangular) {
		const uint8_t *in = src;
		uint8_t *out = s->bmap;

		for (unsigned int y = 0; y < s->sh; y++)
			memcpy(out + y * dst_pitch, in + y * pitch, row_bytes);
		in += (size_t)pitch * ALIGN_TO(height, 32);
		out += (size_t)dst_pitch * ALIGN_TO(s->sh, 32);
		for (unsigned int y = 0; y < (s->sh + 1) / 2; y++)
			memcpy(out + y * dst_pitch, in + y * pitch, row_bytes);
	} else {
		memcpy(s->bmap, src, size);
	}
	s->initialized = 1;
	s->own_layout = rectangular;
	if (ctx->stats_enabled) {
		ctx->stats_copy_ns += vpu_monotonic_ns() - start;
		ctx->stats_copy_bytes += size;
		ctx->stats_copy_frames++;
	}

	if (sync_started) {
		start = ctx->stats_enabled ? vpu_monotonic_ns() : 0;
		ret = surface_finish_write(ctx->session, s);
		if (ctx->stats_enabled)
			ctx->stats_sync_ns += vpu_monotonic_ns() - start;
	}
	return ret;
}

/* Copy one backing into another (same linear layout).  Both mappings are
 * cache-synchronised for the transfer; memfd fallbacks reject the ioctl and
 * are simply coherent CPU mappings. */
static int
surface_backing_copy(struct vpu_surface *dst, struct vpu_surface *src)
{
	size_t size = (size_t)surface_pitch(dst->sw, dst->fourcc) *
		      ALIGN_TO(dst->sh, 32) * 3 / 2;
	int src_sync, dst_sync;

	if (size > dst->bsize || size > src->bsize)
		return 0;
	src_sync = dma_buf_cpu_sync(src->bfd,
				   DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ) == 0;
	dst_sync = dma_buf_cpu_sync(dst->bfd,
				   DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE) == 0;
	memcpy(dst->bmap, src->bmap, size);
	if (dst_sync)
		(void)dma_buf_cpu_sync(dst->bfd,
				       DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
	if (src_sync)
		(void)dma_buf_cpu_sync(src->bfd,
				       DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
	return 1;
}

/* Abandon the current stream: bump the display-level epoch and drop the
 * repeat-last-frame source.  Surfaces written in the old epoch are now stale
 * and will be backfilled lazily if the client presents them without decoding
 * into them first. */
void
surfs_begin_epoch(struct vpu_surfaces *t)
{
	if (!t)
		return;
	t->epoch++;
	if (!t->epoch)
		t->epoch = 1;
	t->last_frame = 0;
	t->last_frame_epoch = 0;
	DBG("[epoch] begin -> %llu\n", (unsigned long long)t->epoch);
}

/* Make a stale surface presentable: repeat the newest frame of the current
 * epoch when one exists, otherwise neutral black.  This is the last line of
 * defence against a client (Chrome reusing its frame pool across per-seek
 * context recreation) sampling pre-seek pixels. */
static void
surfs_backfill_stale(struct vpu_surfaces *t, struct vpu_surface *dst)
{
	struct vpu_surface *src = NULL;
	unsigned int pitch = surface_pitch(dst->sw, dst->fourcc);
	int copied = 0;

	if (t->last_frame_epoch == t->epoch) {
		src = surfs_find(t, t->last_frame);
		if (src && src != dst && src->epoch == t->epoch && src->decoded &&
		    src->fourcc == dst->fourcc && src->bsize == dst->bsize &&
		    surface_pitch(src->sw, src->fourcc) == pitch &&
		    ALIGN_TO(src->sh, 32) == ALIGN_TO(dst->sh, 32))
			copied = surface_backing_copy(dst, src);
	}
	if (!copied)
		surface_fill_black(dst->bfd, dst->bmap, pitch,
				   ALIGN_TO(dst->sh, 32), dst->fourcc);
	dst->epoch = t->epoch;
	dst->decoded = 1;
	dst->initialized = 1;
	DBG("[stale] backfill id=%u %s\n", dst->id,
	    copied ? "last-frame" : "black");
}

/* Retain only VP9 sessions whose reference surfaces still exist. */
int
vpu_decode_retain_vp9(struct vpu_decode_ctx *ctx)
{
	if (!ctx || ctx->codec != VPU_CODEC_VP9 || !ctx->dec_started ||
	    ctx->eos_sent || ctx->fatal_error || ctx->direct_capture || !ctx->surfs)
		return 0;
	for (int i = 0; i < ctx->surfs->n; i++)
		if (ctx->surfs->s[i].owner == ctx)
			return 1;
	return 0;
}

struct vpu_decode_ctx *
vpu_decode_vp9_predecessor(struct vpu_decode_ctx *ctx,
			   const VADecPictureParameterBufferVP9 *pic)
{
	struct vpu_decode_ctx *owner = NULL;
	unsigned int refs[] = { pic->pic_fields.bits.last_ref_frame,
		pic->pic_fields.bits.golden_ref_frame, pic->pic_fields.bits.alt_ref_frame };

	if (ctx->codec != VPU_CODEC_VP9 || ctx->dec_open || ctx->direct_capture ||
	    !pic->pic_fields.bits.frame_type || pic->pic_fields.bits.intra_only)
		return NULL;
	for (unsigned int i = 0; i < ARRAY_SIZE(refs); i++) {
		struct vpu_surface *s = find_surface(ctx, pic->reference_frames[refs[i]]);

		if (!s || !s->owner || (owner && owner != s->owner))
			return NULL;
		owner = s->owner;
	}
	if (!vpu_decode_retain_vp9(owner) || owner->pixel_format != ctx->pixel_format ||
	    ctx->width > owner->width || ctx->height > owner->height)
		return NULL;
	return owner;
}

/* ---- Display-level surface registry ---- */

struct vpu_surfaces *
vpu_surfaces_create(void)
{
	struct vpu_surfaces *t = calloc(1, sizeof(*t));

	if (t && getenv("VPU_VULKAN_COPY"))
		t->vk_copy = vpu_vk_copy_create();
	return t;
}

void
vpu_surfaces_destroy(struct vpu_surfaces *t)
{
	int i;

	if (!t)
		return;
	for (i = 0; i < t->n; i++) {
		struct vpu_surface *s = &t->s[i];

		if (s->owner)
			pthread_mutex_lock(&s->owner->mutex);
		if (s->owner)
			(void)finish_vk_surface(s->owner, s->id, 1);
		if (s->owner)
			vpu_vk_copy_forget(s->owner->vk_copy,
					    s->backing_serial);
		if (s->owner && s->owner->dec_open &&
		    (s->write_started || s->fence_token))
			surface_finish_write(s->owner->session, s);
		munmap(t->s[i].bmap, t->s[i].bsize);
		close(t->s[i].bfd);
		if (s->owner)
			pthread_mutex_unlock(&s->owner->mutex);
	}
	vpu_vk_copy_destroy(t->vk_copy);
	free(t);
}

int
vpu_surfaces_alloc(struct vpu_surfaces *t, VASurfaceID id,
		 unsigned int width, unsigned int height, unsigned int fourcc)
{
	struct vpu_surface *s;
	unsigned int size;
	int heap, bfd;
	void *map;

	if (!t || t->n >= VPU_MAX_SURFACES)
		return -1;

	/* Stable, exportable backing buffer independent of any V4L2 session.
	 * Prefer a real DMA-heap buffer so the exported fd can be imported by
	 * GPU clients (Chrome/EGL); fall back to a plain memfd when the heap
	 * node is root-only, which keeps local tests and CPU readback working.
	 * Size with the negotiated linear NV12/P010 layout (128-byte
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
	s->exported = 0;
	s->initialized = 0;
	s->write_started = 0;
	s->fence_token = 0;
	s->generation = 0;
	s->epoch = t->epoch;
	s->backing_serial = __atomic_add_fetch(&g_buffer_serial, 1,
					       __ATOMIC_RELAXED);
	s->owner = NULL;
	return 0;
}

void
vpu_surfaces_free(struct vpu_surfaces *t, VASurfaceID id)
{
	int i;

	if (!t)
		return;
	for (i = 0; i < t->n; i++) {
		struct vpu_surface *s = &t->s[i];

		if (s->id != id)
			continue;
		if (s->owner)
			pthread_mutex_lock(&s->owner->mutex);
		if (s->owner)
			(void)finish_vk_surface(s->owner, s->id, 1);
		if (s->owner)
			vpu_vk_copy_forget(s->owner->vk_copy,
					    s->backing_serial);
		if (s->owner && s->owner->dec_open &&
		    (s->write_started || s->fence_token))
			surface_finish_write(s->owner->session, s);
		munmap(s->bmap, s->bsize);
		close(s->bfd);
		if (s->owner)
			pthread_mutex_unlock(&s->owner->mutex);
		t->s[i] = t->s[t->n - 1];
		t->n--;
		return;
	}
}

/* Actual NV12 layout of @s: what its owning engine negotiated for CAPTURE,
 * or the aligned coded size before a session exists.  Export/derive must
 * describe buffers with these values, never with assumptions, or clients
 * read garbled rows. */
void
surface_layout(const struct vpu_surface *s, unsigned int *pitch,
	       unsigned int *width, unsigned int *height)
{
	unsigned int p = surface_pitch(s->sw, s->fourcc);
	unsigned int w = s->sw;
	unsigned int h = ALIGN_TO(s->sh, 32);

	if (!s->own_layout && s->owner && s->owner->dec_open) {
		unsigned int negotiated_pitch = 0;
		unsigned int negotiated_width = 0;
		unsigned int negotiated_height = 0;

		vpu_platform_session_capture_layout(s->owner->session, &negotiated_pitch,
					   &negotiated_width,
					   &negotiated_height);
		if (negotiated_pitch && negotiated_width && negotiated_height) {
			p = negotiated_pitch;
			w = negotiated_width;
			h = negotiated_height;
		}
	}
	*pitch = p;
	*width = w;
	*height = h;
}

static void
surface_initialize(struct vpu_surface *s)
{
	unsigned int pitch, width, height;

	if (s->initialized)
		return;
	surface_layout(s, &pitch, &width, &height);
	surface_fill_black(s->bfd, s->bmap, pitch, height, s->fourcc);
	s->initialized = 1;
}

int
vpu_surfaces_sync(struct vpu_surfaces *t, VASurfaceID id)
{
	struct vpu_surface *s = surfs_find(t, id);

	/* Never-queued and already-decoded surfaces succeed without
	 * draining anything: Chrome syncs freshly allocated pool surfaces
	 * before exporting them and must not get spurious timeouts. */
	if (!s)
		return -EINVAL;
	/* A surface that has not been decoded into during the current stream
	 * epoch still holds pre-seek pixels.  Backfill it with the newest frame
	 * of this epoch (or black) before the client can present it. */
	if (s->epoch != t->epoch) {
		surfs_backfill_stale(t, s);
		return 0;
	}
	if (!s->queued || s->decoded)
		return 0;
	/* Drain whichever engine queued this picture; with one engine per
	 * context that is exactly the context still holding the stream. */
	if (s->owner) {
		int r = vpu_decode_sync(s->owner, id);

		DBG("[surfs_sync] id=%u -> engine sync r=%d\n", id, r);
		return r;
	}
	return 0;
}

int
vpu_surfaces_ready(struct vpu_surfaces *t, VASurfaceID id)
{
	struct vpu_surface *s = surfs_find(t, id);

	return s ? s->decoded : 0;
}

int
vpu_surfaces_valid(struct vpu_surfaces *t, VASurfaceID id)
{
	return surfs_find(t, id) != NULL;
}

int
vpu_surfaces_export(struct vpu_surfaces *t, VASurfaceID id, int *fd,
		  unsigned int *pitch, unsigned int *size,
		  unsigned int *width, unsigned int *height,
		  unsigned int *fourcc)
{
	struct vpu_surface *s = surfs_find(t, id);
	int exported_fd;
	unsigned int p, w, h;

	if (!s)
		return -1;
	/* VA surface contents are not observable until export/derive/get-image.
	 * Keep decode-only pools lazy so allocating dozens of 4K P010 surfaces
	 * does not write gigabytes of neutral-black pixels that no client reads. */
	surface_initialize(s);
	surface_layout(s, &p, &w, &h);
	/* vaExportSurfaceHandle transfers ownership of every returned object
	 * fd to the caller.  Keep the driver's backing fd private: Chrome
	 * closes the exported fd after importing it, and returning s->bfd
	 * directly caused a double-close/FD-reuse crash in the GPU process. */
	exported_fd = fcntl(s->bfd, F_DUPFD_CLOEXEC, 0);
	if (exported_fd < 0)
		return -1;
	/* If this backing is reused for a later picture, legacy Adreno may have
	 * imported it before the new reservation fence was attached. */
	s->exported = 1;
	*fd = exported_fd;
	*pitch = p;
	*size = s->bsize;
	*width = w;
	*height = h;
	*fourcc = s->fourcc;
	return 0;
}

static int
surfaces_buffer(struct vpu_surfaces *t, VASurfaceID id, void **mem,
		unsigned int *pitch, unsigned int *size,
		unsigned int *width, unsigned int *height,
		unsigned int *fourcc, int initialize)
{
	struct vpu_surface *s = surfs_find(t, id);
	unsigned int p, w, h;

	if (!s)
		return -1;
	if (initialize)
		surface_initialize(s);
	surface_layout(s, &p, &w, &h);
	*mem = s->bmap;
	*pitch = p;
	*size = s->bsize;
	*width = w;
	*height = h;
	*fourcc = s->fourcc;
	return 0;
}

int
vpu_surfaces_buffer(struct vpu_surfaces *t, VASurfaceID id, void **mem,
		  unsigned int *pitch, unsigned int *size,
		  unsigned int *width, unsigned int *height,
		  unsigned int *fourcc)
{
	return surfaces_buffer(t, id, mem, pitch, size, width, height, fourcc, 1);
}

int
vpu_surfaces_peek_buffer(struct vpu_surfaces *t, VASurfaceID id, void **mem,
		  unsigned int *pitch, unsigned int *size,
		  unsigned int *width, unsigned int *height,
		  unsigned int *fourcc)
{
	return surfaces_buffer(t, id, mem, pitch, size, width, height, fourcc, 0);
}
