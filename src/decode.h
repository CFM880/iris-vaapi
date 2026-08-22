// SPDX-License-Identifier: GPL-2.0-or-later
/* Decode backend for the iris-vaapi driver: wraps the V4L2 engine, tracks VA
 * surfaces, accumulates picture parameters + slices, reassembles a whole H.264
 * access unit and maps decoded NV12 CAPTURE buffers back to VA surfaces. */

#ifndef IRIS_VAAPI_DECODE_H
#define IRIS_VAAPI_DECODE_H

#include <stdint.h>
#include <stddef.h>
#include <va/va.h>

struct iris_decode_ctx;

struct iris_decode_ctx *iris_decode_create(void);
void iris_decode_destroy(struct iris_decode_ctx *ctx);

void iris_decode_setup(struct iris_decode_ctx *ctx, unsigned int width,
		       unsigned int height, VAProfile profile);

/* Surface lifecycle. */
int iris_decode_create_surface(struct iris_decode_ctx *ctx, VASurfaceID id);
int iris_decode_destroy_surface(struct iris_decode_ctx *ctx, VASurfaceID id);
int iris_decode_surface_ready(struct iris_decode_ctx *ctx, VASurfaceID id);

/* Per-picture accumulation (called from vaRenderPicture). */
int iris_decode_begin(struct iris_decode_ctx *ctx, VASurfaceID target);
int iris_decode_picture(struct iris_decode_ctx *ctx,
			const VAPictureParameterBufferH264 *pic);
/* Capture per-slice parameters (reference counts) for the PPS default. */
int iris_decode_slice_params(struct iris_decode_ctx *ctx,
			     const VASliceParameterBufferH264 *sp);
int iris_decode_slice(struct iris_decode_ctx *ctx, const void *data,
		      size_t len);

/* Reassemble and feed the engine.  Returns once queued (async). */
int iris_decode_end(struct iris_decode_ctx *ctx);
/* Wait until @id has a decoded frame (drains the engine). */
int iris_decode_sync(struct iris_decode_ctx *ctx, VASurfaceID id);

/* Export the decoded buffer of @id as a DRM PRIME fd. */
int iris_decode_export(struct iris_decode_ctx *ctx, VASurfaceID id, int *fd,
		       unsigned int *pitch, unsigned int *size,
		       unsigned int *width, unsigned int *height);

/* Look up the decoded CAPTURE buffer of @id (for vaDeriveImage/vaMapBuffer). */
int iris_decode_surface_buffer(struct iris_decode_ctx *ctx, VASurfaceID id,
			       unsigned int *cap_index, void **mem,
			       unsigned int *pitch, unsigned int *size,
			       unsigned int *width, unsigned int *height);

#endif