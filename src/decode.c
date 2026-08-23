// SPDX-License-Identifier: GPL-2.0-or-later

#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <linux/dma-heap.h>

#include "decode.h"
#include "h264_params.h"
#include "hevc_params.h"
#include "v4l2_dec.h"

#define IRIS_MAX_SURFACES	64
#define IRIS_AU_MAX		(16U * 1024 * 1024)

#ifndef ALIGN_TO
#define ALIGN_TO(x, a) (((x) + (a) - 1) & ~((a) - 1))
#endif

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC	0x0001U
#endif

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

struct iris_surface {
	VASurfaceID id;
	int bfd;		/* backing fd (DMA-heap, or memfd fallback) */
	void *bmap;		/* mmap of the backing */
	unsigned int bsize;
	int decoded;		/* a frame has been copied into the backing */
	int queued;		/* some picture was decoded into this surface */
};

struct iris_decode_ctx {
	struct v4l2_dec dec;
	int dec_open;
	int dec_started;
	unsigned int width, height;
	VAProfile profile;
	unsigned int out_pixfmt;	/* V4L2 OUTPUT pixel format */

	struct iris_surface surfaces[IRIS_MAX_SURFACES];
	int n_surfaces;

	VAPictureParameterBufferH264 pic;
	int have_pic;
	VAPictureParameterBufferHEVC hevc_pic;
	int have_hevc_pic;
	uint8_t *slice_data;
	size_t slice_len;
	size_t slice_cap;
	VASurfaceID current_target;

	uint8_t last_sps[256];
	int last_sps_len;
	uint8_t last_pps[128];
	int last_pps_len;
	/* HEVC parameter-set cache (only emit when changed). */
	uint8_t last_hevc_vps[64];
	int last_hevc_vps_len;
	uint8_t last_hevc_sps[256];
	int last_hevc_sps_len;
	uint8_t last_hevc_pps[128];
	int last_hevc_pps_len;
	int refs_l0, refs_l1;	/* from slice params, for the PPS default */

	/* Map decode sequence numbers back to target surfaces so frame
	 * matching does not depend on the (possibly non-contiguous) VASurfaceID
	 * values that the client happens to use. */
	uint64_t seq;
	VASurfaceID target_of[512];
	VASurfaceID last_target;	/* most recently queued picture */
	int eos_sent;			/* EOS (v4l2_dec_flush) queued */
	/* HEVC firmware does not propagate per-frame timestamps; all dequeued
	 * frames carry the same value.  Match frames to surfaces by output
	 * order instead. */
	uint64_t next_out_seq;
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

int
iris_decode_create_surface(struct iris_decode_ctx *ctx, VASurfaceID id)
{
	struct iris_surface *s;
	unsigned int size;
	int heap, bfd;
	void *map;

	if (ctx->n_surfaces >= 64)
		return -1;

	/* Stable, exportable backing buffer independent of the V4L2 session.
	 * Prefer a real DMA-heap buffer so the exported fd can be imported by
	 * GPU clients (Chrome/EGL); fall back to a plain memfd when the heap
	 * node is root-only, which keeps local tests and CPU readback working. */
	size = ALIGN_TO(ctx->width, 16) * ALIGN_TO(ctx->height, 32) * 3 / 2;
	fprintf(stderr, "[surf] id=%u size=%u w=%u h=%u\n", id, size,
		ctx->width, ctx->height);
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

	s = &ctx->surfaces[ctx->n_surfaces++];
	s->id = id;
	s->bfd = bfd;
	s->bmap = map;
	s->bsize = size;
	s->decoded = 0;
	return 0;
}

int
iris_decode_destroy_surface(struct iris_decode_ctx *ctx, VASurfaceID id)
{
	int i;

	for (i = 0; i < ctx->n_surfaces; i++) {
		struct iris_surface *s = &ctx->surfaces[i];

		if (s->id != id)
			continue;
		munmap(s->bmap, s->bsize);
		close(s->bfd);
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

	if (ctx->dec_open) {
		/* After an EOS flush the firmware is done; a client that keeps
		 * decoding (Chrome flush/reset, looped playback) needs a fresh
		 * session. */
		if (ctx->eos_sent) {
			v4l2_dec_close(&ctx->dec);
			ctx->dec_open = 0;
			ctx->dec_started = 0;
			ctx->eos_sent = 0;
		} else {
			return 0;
		}
	}
	ret = v4l2_dec_open(&ctx->dec, "/dev/video0", ctx->width,
			    ctx->height, ctx->out_pixfmt);
	if (ret)
		return ret;
	ctx->dec_open = 1;
	return 0;
}


/* Assign one dequeued frame to its surface.  For H.264/VP9 the firmware
 * propagates the timestamp we encoded with the surface id; for HEVC it
 * stamps every frame with the same value, so fall back to output-order
 * matching.  Returns the surface id, or -1 if unknown. */
static int
assign_frame(struct iris_decode_ctx *ctx, const struct v4l2_dec_frame *frame)
{
	uint64_t seq;
	VASurfaceID id;
	struct iris_surface *s;

	if (ctx->out_pixfmt == V4L2_PIX_FMT_HEVC) {
		seq = ctx->next_out_seq++;
		if (seq >= 512)
			return -1;
	} else {
		seq = (frame->timestamp / 1000000000ULL) - 1000;
		if (seq >= 512)
			return -1;
	}
	id = ctx->target_of[seq];
	s = find_surface(ctx, id);

	if (!s) {
		v4l2_dec_qcap_idx(&ctx->dec, frame->index);
		return -1;
	}
	/* Copy the decoded frame into the surface's stable DMA-heap backing so
	 * buffers exported before decoding stay valid, then recycle the
	 * firmware buffer. */
	if (frame->bytesused <= s->bsize)
		memcpy(s->bmap, frame->mem, frame->bytesused);
	v4l2_dec_qcap_idx(&ctx->dec, frame->index);
	s->decoded = 1;
	s->queued = 1;
	return id;
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
		return 0;

	ret = v4l2_dec_flush(&ctx->dec);
	if (ret)
		return ret;
	ctx->eos_sent = 1;

	/* Drain until the EOS frame (V4L2_BUF_FLAG_LAST) is dequeued.  Its
	 * timestamp is the last AU's timestamp, so it maps to the right
	 * surface; if the firmware reports the empty flush buffer's zero
	 * timestamp instead, fall back to the last queued target. */
	while (deadline-- > 0) {
		int changed;

		ret = v4l2_dec_poll(&ctx->dec, 20);
		if (ret <= 0)
			continue;
		ret = v4l2_dec_handle_events(&ctx->dec, &changed);
		if (ret)
			return ret;
		while (v4l2_dec_dqout(&ctx->dec) == 0)
			;
		ret = v4l2_dec_dqcap(&ctx->dec, &frame);
		if (ret < 0)
			continue;
		fprintf(stderr, "[flush] got ts=%llu flags=0x%x\n",
			(unsigned long long)frame.timestamp, frame.flags);
		if (assign_frame(ctx, &frame) < 0 && ctx->last_target) {
			struct iris_surface *s = find_surface(ctx,
							       ctx->last_target);
			if (s) {
				if (frame.bytesused <= s->bsize)
					memcpy(s->bmap, frame.mem,
					       frame.bytesused);
				v4l2_dec_qcap_idx(&ctx->dec, frame.index);
				s->decoded = 1;
			}
		}
		if (ret == 1)
			break;
	}
	return 0;
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
	fprintf(stderr, "[begin] target=%u\n", target);
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

int
iris_decode_hevc_picture(struct iris_decode_ctx *ctx,
			 const VAPictureParameterBufferHEVC *pic)
{
	memcpy(&ctx->hevc_pic, pic, sizeof(*pic));
	ctx->have_hevc_pic = 1;
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

	fprintf(stderr, "[end] target=%u slice_len=%zu refs=%d/%d started=%d\n",
		ctx->current_target, ctx->slice_len, ctx->refs_l0, ctx->refs_l1,
		ctx->dec_started);
	size_t au_len = 0, au_cap;
	int n, ret;
	static const uint8_t sc4[4] = { 0, 0, 0, 1 };

	if (!ctx->have_pic && ctx->out_pixfmt != V4L2_PIX_FMT_VP9 &&
	    !ctx->have_hevc_pic)
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

	/* Assemble the access unit to feed the stateful firmware.
	 *
	 * H.264/HEVC: the client (Chrome/ffmpeg) sends picture/slice parameter
	 * buffers but no parameter-set NALs, so re-serialize SPS/PPS (or
	 * VPS/SPS/PPS) and prepend them, only when they change.
	 *
	 * VP9: each frame is self-contained (its own uncompressed+compressed
	 * header); the slice data is the whole frame, feed it verbatim. */
	if (ctx->out_pixfmt == V4L2_PIX_FMT_VP9) {
		au_len = ctx->slice_len;
		memcpy(au, ctx->slice_data, au_len);
	} else if (ctx->out_pixfmt == V4L2_PIX_FMT_HEVC) {
		/* Re-serialize VPS/SPS/PPS from the picture params and prepend
		 * only when they change (per-picture repetition resets the
		 * firmware DPB).  ffmpeg/Chrome send bare slice NALs. */
		n = hevc_build_vps(au + 4, au_cap - 4 - ctx->slice_len,
				   &ctx->hevc_pic);
		if (n > 0 && (n != ctx->last_hevc_vps_len ||
			      memcmp(au + 4, ctx->last_hevc_vps, n) != 0)) {
			memcpy(au, sc4, 4);
			memcpy(ctx->last_hevc_vps, au + 4, n);
			ctx->last_hevc_vps_len = n;
			au_len = 4 + n;
		}
		n = hevc_build_sps(au + au_len + 4, au_cap - au_len - 4 -
				   ctx->slice_len, &ctx->hevc_pic);
		if (n > 0 && (n != ctx->last_hevc_sps_len ||
			      memcmp(au + au_len + 4, ctx->last_hevc_sps, n) != 0)) {
			memcpy(au + au_len, sc4, 4);
			memcpy(ctx->last_hevc_sps, au + au_len + 4, n);
			ctx->last_hevc_sps_len = n;
			au_len += 4 + n;
		}
		n = hevc_build_pps(au + au_len + 4, au_cap - au_len - 4 -
				   ctx->slice_len, &ctx->hevc_pic);
		if (n > 0 && (n != ctx->last_hevc_pps_len ||
			      memcmp(au + au_len + 4, ctx->last_hevc_pps, n) != 0)) {
			memcpy(au + au_len, sc4, 4);
			memcpy(ctx->last_hevc_pps, au + au_len + 4, n);
			ctx->last_hevc_pps_len = n;
			au_len += 4 + n;
		}
		if (au_len + ctx->slice_len > au_cap) {
			free(au);
			return -1;
		}
		memcpy(au + au_len, ctx->slice_data, ctx->slice_len);
		au_len += ctx->slice_len;
	} else {
	/* Only re-emit SPS/PPS when they change; a per-picture repetition
	 * resets the firmware DPB and breaks P-frame references. */
	n = h264_build_sps(au + 4, au_cap - 4 - ctx->slice_len, &ctx->pic,
			   ctx->profile);
	if (n <= 0) {
		free(au);
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
	if (n <= 0) {
		free(au);
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
		free(au);
		return -1;
	}
	memcpy(au + au_len, ctx->slice_data, ctx->slice_len);
	au_len += ctx->slice_len;
	}

	{
		uint64_t ts = (ctx->seq + 1000) * 1000000000ULL;

		ctx->last_target = ctx->current_target;
		{
			struct iris_surface *qt = find_surface(ctx,
							       ctx->current_target);
			if (qt)
				qt->queued = 1;
		}
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
	rv = 0;
	fprintf(stderr, "[end] done rv=%d\n", rv);
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
	if (s && !s->queued)
		return 0;

	if (iris_decode_surface_ready(ctx, id))
		return 0;

	/* The stateful firmware holds the last queued picture until an EOS
	 * marker arrives.  If the client is syncing that final picture, feed
	 * EOS to force it out instead of spinning until the timeout. */
	if (id == ctx->last_target && !ctx->eos_sent) {
		fprintf(stderr, "[sync] last_target=%u: flushing\n", id);
		if (iris_decode_flush(ctx) == 0 &&
		    iris_decode_surface_ready(ctx, id))
			return 0;
	}

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
		fprintf(stderr, "[sync] got ts=%llu\n",
			(unsigned long long)frame.timestamp);
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

	if (!s)
		return -1;
	*cap_index = 0;
	*mem = s->bmap;
	*pitch = ALIGN_TO(ctx->width, 16);
	*size = s->bsize;
	*width = ctx->width;
	*height = ctx->height;
	return 0;
}

int
iris_decode_export(struct iris_decode_ctx *ctx, VASurfaceID id, int *fd,
		   unsigned int *pitch, unsigned int *size,
		   unsigned int *width, unsigned int *height)
{
	struct iris_surface *s = find_surface(ctx, id);

	if (!s)
		return -1;
	*fd = s->bfd;
	*pitch = ALIGN_TO(ctx->width, 16);
	*size = s->bsize;
	*width = ctx->width;
	*height = ctx->height;
	return 0;
}