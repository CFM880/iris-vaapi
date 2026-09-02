// SPDX-License-Identifier: GPL-2.0-or-later
/* Platform-neutral decode core for vpu-vaapi: drives a platform session, tracks VA
 * surfaces, accumulates picture parameters + slices, reassembles complete
 * access units and maps decoded frames back to VA surfaces.
 *
 * Threading/lifecycle model (matches how Chrome uses libva):
 * - Surfaces live in a display-level registry (vpu_surfaces) and may outlive
 *   the decode context that produced them.
 * - Each VA context owns its own vpu_decode_ctx (its own VPU session), so
 *   concurrent videos do not share engine state and switching codec or
 *   resolution always starts from a clean firmware session.
 */

#ifndef VPU_VAAPI_DECODE_H
#define VPU_VAAPI_DECODE_H

#include <stdint.h>
#include <stddef.h>
#include <va/va.h>

struct vpu_decode_ctx;
struct vpu_surfaces;
struct vpu_platform;

/* ---- Display-level surface registry ---- */
struct vpu_surfaces *vpu_surfaces_create(void);
void vpu_surfaces_destroy(struct vpu_surfaces *t);
int vpu_surfaces_alloc(struct vpu_surfaces *t, VASurfaceID id,
		     unsigned int width, unsigned int height,
		     unsigned int fourcc);
void vpu_surfaces_free(struct vpu_surfaces *t, VASurfaceID id);
/* Sync routing: drains the owning engine until @id is ready.  Never-queued,
 * already-decoded surfaces and surfaces whose engine is gone succeed
 * immediately (Chrome syncs pool surfaces before decoding into them). */
int vpu_surfaces_sync(struct vpu_surfaces *t, VASurfaceID id);
/* Advisory readiness without draining an engine. */
int vpu_surfaces_ready(struct vpu_surfaces *t, VASurfaceID id);
int vpu_surfaces_valid(struct vpu_surfaces *t, VASurfaceID id);
/* Export the backing of @id as a fresh DRM PRIME fd (caller owns it). */
int vpu_surfaces_export(struct vpu_surfaces *t, VASurfaceID id, int *fd,
		      unsigned int *pitch, unsigned int *size,
		      unsigned int *width, unsigned int *height,
		      unsigned int *fourcc);
/* Backing memory layout of @id (for vaDeriveImage/vaGetImage). */
int vpu_surfaces_buffer(struct vpu_surfaces *t, VASurfaceID id, void **mem,
		      unsigned int *pitch, unsigned int *size,
		      unsigned int *width, unsigned int *height,
		      unsigned int *fourcc);

/* ---- Per-context decode engines ---- */
struct vpu_decode_ctx *vpu_decode_create(const struct vpu_platform *platform);
void vpu_decode_destroy(struct vpu_decode_ctx *ctx);

int vpu_decode_setup(struct vpu_decode_ctx *ctx, unsigned int width,
		      unsigned int height, VAProfile profile);

/* Attach the display-level registry this engine reads/writes surfaces in. */
void vpu_decode_set_surfaces(struct vpu_decode_ctx *ctx,
			      struct vpu_surfaces *t);
/* Bind context render targets for the opt-in direct CAPTURE DMA-BUF ring. */
int vpu_decode_set_render_targets(struct vpu_decode_ctx *ctx,
				   const VASurfaceID *targets,
				   unsigned int count);
/* Legacy shims: allocate/free a surface in the attached registry. */
int vpu_decode_create_surface(struct vpu_decode_ctx *ctx, VASurfaceID id);
void vpu_decode_destroy_surface(struct vpu_decode_ctx *ctx, VASurfaceID id);

/* Update coded dimensions of a context that has not started decoding. */
void vpu_decode_reconfigure(struct vpu_decode_ctx *ctx, unsigned int width,
			     unsigned int height);
/* Tear down the firmware session and per-stream state (seek/reset). */
void vpu_decode_reset(struct vpu_decode_ctx *ctx);

/* Wait until @id has a decoded frame (drains this engine). */
int vpu_decode_sync(struct vpu_decode_ctx *ctx, VASurfaceID id);
/* Feed EOS so the firmware releases the picture it is holding. */
int vpu_decode_flush(struct vpu_decode_ctx *ctx);

/* Per-picture codec accumulation (called from vaRenderPicture). */
int vpu_decode_begin(struct vpu_decode_ctx *ctx, VASurfaceID target);
int vpu_decode_render(struct vpu_decode_ctx *ctx, VABufferType type,
		       const void *data, size_t size, unsigned int elements);

/* Reassemble and feed the engine.  Returns once queued (async). */
int vpu_decode_end(struct vpu_decode_ctx *ctx);

#endif
