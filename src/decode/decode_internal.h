// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef VPU_VAAPI_DECODE_INTERNAL_H
#define VPU_VAAPI_DECODE_INTERNAL_H

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#include "decode.h"
#include "codec/codec.h"
#include "codec/types.h"
#include "platform/platform.h"
#include "vk_copy.h"

/* Chrome runs one VaapiVideoDecoder per video; every decoder owns a frame
 * pool of up to ~32 surfaces and pools coexist across tabs/resolution
 * changes, so the registry must hold several pools at once. */
#define VPU_MAX_SURFACES	128
#define VPU_MAX_PENDING_COPIES	32

/* Map decode sequence numbers back to target surfaces so frame matching does
 * not depend on the (possibly non-contiguous) VASurfaceID values that the
 * client happens to use.  The ring is indexed by (seq & mask) and validated by
 * the stored seq, so playback longer than any fixed table just wraps instead
 * of breaking. */
#define VPU_TARGET_RING	1024	/* power of two */
/* Platforms with HEVC_CAPTURE_FIFO do not propagate usable per-frame
 * timestamps, so retain POC-to-surface state as a strict FIFO ring. */
#define VPU_HEVC_RING	512

#ifndef ALIGN_TO
#define ALIGN_TO(x, a) (((x) + (a) - 1) & ~((a) - 1))
#endif
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

struct vpu_surface {
	VASurfaceID id;
	int bfd;		/* backing fd (DMA-heap, or memfd fallback) */
	void *bmap;		/* mmap of the backing */
	unsigned int bsize;
	unsigned int sw, sh;	/* coded size the backing was created for */
	unsigned int fourcc;	/* VA/V4L2 layout: NV12 or P010 */
	int decoded;		/* a frame has been copied into the backing */
	int queued;		/* some picture was decoded into this surface */
	int exported;		/* backing has been exported to a DRM client */
	int initialized;	/* backing has decoded or neutral-black pixels */
	int own_layout;		/* visible rectangle copied into surface layout */
	int write_started;	/* DMA_BUF_SYNC write access spans async decode */
	uint64_t fence_token;	/* pending kernel reservation fence, or zero */
	uint64_t generation;	/* render-target reuse generation */
	uint64_t epoch;		/* stream epoch of the last valid backing content */
	uint64_t backing_serial;	/* unique identity for Vulkan import cache */
	struct vpu_decode_ctx *owner;	/* engine that queued the picture */
};

/* Display-level registry: surfaces may outlive the engine that decodes into
 * them (Chrome destroys contexts on navigation while pool surfaces drain). */
struct vpu_surfaces {
	struct vpu_surface s[VPU_MAX_SURFACES];
	int n;
	struct vpu_vk_copy *vk_copy;
	/* Stream epoch: bumped whenever the decoder abandons a stream (seek /
	 * context recreation).  A surface whose backing was last written in an
	 * older epoch still holds pre-seek pixels and must not be presented.
	 * last_frame/last_frame_epoch track the newest frame decoded in the
	 * current epoch so a stale surface can be backfilled (repeat last
	 * frame) instead of revealing the old picture. */
	uint64_t epoch;
	VASurfaceID last_frame;
	uint64_t last_frame_epoch;
};

struct vpu_decode_ctx {
	pthread_mutex_t mutex;
	struct vpu_platform_session *session;
	struct vpu_codec *codec_adapter;
	int dec_open;
	int dec_started;
	unsigned int width, height;
	enum vpu_codec_id codec;
	enum vpu_pixel_format pixel_format;
	uint32_t platform_quirks;
	int direct_capture;
	int direct_error;
	int fatal_error;
	struct vpu_vk_copy *vk_copy;
	int vk_copy_failed;
	uint64_t vk_capture_generation;
	uint64_t vk_capture_keys[VPU_MAX_SURFACES];
	struct {
		struct vpu_vk_job *job;
		VASurfaceID id;
		uint64_t generation;
		uint64_t fence_token;
		uint64_t start_ns;
		size_t bytes;
		unsigned int capture_index;
		int used;
	} pending_copies[VPU_MAX_PENDING_COPIES];
	unsigned int direct_count;
	unsigned int direct_requested_count;
	struct {
		VASurfaceID id;
		int fd;
		size_t size;
	} direct[VPU_MAX_SURFACES];

	struct vpu_surfaces *surfs;	/* not owned */
	VASurfaceID current_target;
	uint64_t current_generation;
	/*
	 * H.264 PAFF renders both fields of a frame into the same render target
	 * with two BeginPicture/EndPicture pairs.  field_open tracks a first
	 * field whose second field is still to come, second_field marks the
	 * current picture as that second field, and field_ts keeps the
	 * timestamp shared by the pair so one firmware frame maps back to the
	 * target ring entry created for the first field.
	 */
	int field_open;
	int second_field;
	uint64_t field_ts;

	uint64_t seq;
	struct {
		uint64_t seq;
		VASurfaceID target;
		uint64_t generation;
		int used;
	} target_ring[VPU_TARGET_RING];
	VASurfaceID last_target;	/* most recently queued picture */
	int eos_sent;			/* EOS (vpu_platform_session_flush) queued */
	struct {
		int32_t poc;
		VASurfaceID target;
		uint64_t generation;
	} hevc_ring[VPU_HEVC_RING];
	unsigned int hevc_ring_head, hevc_ring_len;

	int stats_enabled;
	uint64_t stats_copy_ns;
	uint64_t stats_copy_bytes;
	uint64_t stats_copy_frames;
	uint64_t stats_vk_copy_ns;
	uint64_t stats_vk_copy_bytes;
	uint64_t stats_vk_copy_frames;
	uint64_t stats_vk_copy_fallbacks;
	uint64_t stats_capture_frames;
	uint64_t stats_sync_ns;
	uint64_t stats_rewrite_ns;
	uint64_t stats_rewrite_bytes;
	uint64_t stats_rewrites;
	uint64_t stats_end_ns;
	uint64_t stats_ends;
	uint64_t stats_h264_wait_ns;
	uint64_t stats_h264_waits;
	uint64_t stats_h264_async;
	uint64_t stats_direct_frames;
	uint64_t last_submit_ns;
	int vp9_seek_barrier;
	uint8_t vp9_release_au[2];
	size_t vp9_release_len;
	int force_h264_sync_end;
};

/* Shared buffer identity counter (surface backings and Vulkan capture keys). */
extern uint64_t g_buffer_serial;

/* Per-frame tracing is extremely chatty and the GPU process inherits this
 * stderr; unconditionally writing it stalls the decode loop when the terminal
 * is slow.  Opt in with VPU_VAAPI_DEBUG=1. */
int vpu_dbg_enabled(void);
uint64_t vpu_monotonic_ns(void);

#define DBG(...)	do { if (vpu_dbg_enabled()) fprintf(stderr, __VA_ARGS__); } while (0)

/* ---- Surface backing + display registry (surface.c) ---- */
unsigned int surface_pitch(unsigned int width, unsigned int fourcc);
int surface_finish_write(struct vpu_platform_session *session,
			 struct vpu_surface *s);
void surface_begin_write(struct vpu_platform_session *session,
			 struct vpu_surface *s, uint64_t token);
void surface_begin_device_write(struct vpu_platform_session *session,
				struct vpu_surface *s, uint64_t token);
void surfaces_mark_decoded(struct vpu_decode_ctx *ctx, struct vpu_surface *s);
int surface_copy(struct vpu_decode_ctx *ctx, struct vpu_surface *s,
		 const void *src, size_t size);
struct vpu_surface *surfs_find(struct vpu_surfaces *t, VASurfaceID id);
struct vpu_surface *find_surface(struct vpu_decode_ctx *ctx, VASurfaceID id);
void surfs_begin_epoch(struct vpu_surfaces *t);
void surface_layout(const struct vpu_surface *s, unsigned int *pitch,
		    unsigned int *width, unsigned int *height);

/* ---- Vulkan DMA-BUF capture copies (vk_capture.c) ---- */
int reap_vk_copies(struct vpu_decode_ctx *ctx, int wait_all);
void forget_vk_capture_buffers(struct vpu_decode_ctx *ctx);
int finish_vk_surface(struct vpu_decode_ctx *ctx, VASurfaceID id, int wait);
int surface_vk_submit(struct vpu_decode_ctx *ctx, struct vpu_surface *s,
		      const struct vpu_decoded_frame *frame);
void finish_pending_writes(struct vpu_decode_ctx *ctx);

/* ---- Firmware session lifecycle and stream boundaries (stream.c) ---- */
void reset_stream_state(struct vpu_decode_ctx *ctx);
int ensure_decoder(struct vpu_decode_ctx *ctx);
unsigned int direct_collect_surfaces(struct vpu_decode_ctx *ctx);
int direct_surface_index(struct vpu_decode_ctx *ctx, VASurfaceID id);
int stream_boundary_restart(struct vpu_decode_ctx *ctx);
int drain_available(struct vpu_decode_ctx *ctx);
int wait_surface_ready(struct vpu_decode_ctx *ctx, VASurfaceID id,
		       int deadline);

/* ---- Access-unit pipeline (decode.c) ---- */
int assign_frame(struct vpu_decode_ctx *ctx,
		 const struct vpu_decoded_frame *frame);

#endif
