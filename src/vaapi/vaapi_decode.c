// SPDX-License-Identifier: GPL-2.0-or-later
/* VA context and decode path: one vpu_decode_ctx per VAContext, plus the
 * BeginPicture/RenderPicture/EndPicture and surface-sync entrypoints. */

#include "vaapi_internal.h"

static struct vpu_decode_ctx *
vpu_context_engine(struct vpu_drv_data *dd, VAContextID context_id)
{
	int i;

	for (i = 0; i < dd->n_engines; i++)
		if (dd->engines[i].ctx_id == context_id)
			return dd->engines[i].dec;
	return NULL;
}

static struct vpu_engine *
vpu_context(struct vpu_drv_data *dd, VAContextID context_id)
{
	int i;

	for (i = 0; i < dd->n_engines; i++)
		if (dd->engines[i].ctx_id == context_id)
			return &dd->engines[i];
	return NULL;
}

VAStatus
vpu_vaCreateContext(VADriverContextP ctx, VAConfigID config_id, int picture_width,
		     int picture_height, int flag, VASurfaceID *render_targets,
		     int num_render_targets, VAContextID *context_id)
{
	struct vpu_drv_data *dd;
	struct vpu_surfaces *t;
	struct vpu_decode_ctx *eng;
	struct vpu_config *cfg;

	dd = vpu_drv_data(ctx);
	if (!dd)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	cfg = vpu_find_config(dd, config_id);
	if (!cfg)
		return VA_STATUS_ERROR_INVALID_CONFIG;
	t = vpu_ensure_surfs(dd);
	if (!t)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	if (dd->n_engines >= VPU_MAX_ENGINES)
		return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;

	/* Begin with a fresh engine. VP9 RenderPicture may later establish an
	 * explicit dependency on a retired engine through reference surfaces.
	 */
	eng = vpu_decode_create(dd->platform);
	if (!eng)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	if (vpu_decode_setup(eng, picture_width, picture_height,
			      cfg->profile)) {
		vpu_decode_destroy(eng);
		return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
	}
	vpu_decode_set_surfaces(eng, t);
	if (vpu_decode_set_render_targets(eng, render_targets,
					   num_render_targets) < 0) {
		vpu_decode_destroy(eng);
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	}

	dd->width = picture_width;
	dd->height = picture_height;
	dd->engines[dd->n_engines].ctx_id = ++dd->context_id;
	dd->engines[dd->n_engines].dec = eng;
	dd->engines[dd->n_engines].target = VA_INVALID_SURFACE;
	dd->n_engines++;
	*context_id = dd->context_id;
	return VA_STATUS_SUCCESS;
}

VAStatus
vpu_vaDestroyContext(VADriverContextP ctx, VAContextID context_id)
{
	struct vpu_drv_data *dd = ctx->pDriverData;
	int i;

	if (!dd)
		return VA_STATUS_ERROR_INVALID_CONTEXT;
	for (i = 0; i < dd->n_engines; i++) {
		if (dd->engines[i].ctx_id != context_id)
			continue;
		/* VP9 inter-resolution changes can reference surviving surfaces.
		 * Keep a bounded set of their engines until the references disappear
		 * or a new context explicitly takes ownership.
		 */
		if (dd->n_retired_vp9 < VPU_MAX_ENGINES &&
		    vpu_decode_retain_vp9(dd->engines[i].dec))
			dd->retired_vp9[dd->n_retired_vp9++] = dd->engines[i].dec;
		else
			vpu_decode_destroy(dd->engines[i].dec);
		dd->engines[i] = dd->engines[dd->n_engines - 1];
		dd->n_engines--;
		return VA_STATUS_SUCCESS;
	}
	return VA_STATUS_ERROR_INVALID_CONTEXT;
}

VAStatus
vpu_vaBeginPicture(VADriverContextP ctx, VAContextID context_id,
		    VASurfaceID render_target)
{
	struct vpu_drv_data *dd = ctx->pDriverData;
	struct vpu_decode_ctx *eng;

	if (!dd)
		return VA_STATUS_ERROR_INVALID_CONTEXT;
	eng = vpu_context_engine(dd, context_id);
	if (!eng)
		return VA_STATUS_ERROR_INVALID_CONTEXT;
	vpu_context(dd, context_id)->target = render_target;
	return vpu_decode_begin(eng, render_target) ?
		VA_STATUS_ERROR_OPERATION_FAILED : VA_STATUS_SUCCESS;
}

VAStatus
vpu_vaRenderPicture(VADriverContextP ctx, VAContextID context_id,
		     VABufferID *buffers, int num_buffers)
{
	struct vpu_drv_data *dd = ctx->pDriverData;
	struct vpu_engine *engine;
	struct vpu_decode_ctx *eng;
	int i;

	if (!dd)
		return VA_STATUS_ERROR_INVALID_CONTEXT;
	engine = vpu_context(dd, context_id);
	if (!engine)
		return VA_STATUS_ERROR_INVALID_CONTEXT;
	eng = engine->dec;

	for (i = 0; i < num_buffers; i++) {
		int idx = vpu_find_buffer(dd, buffers[i]);
		int ret;

		if (idx < 0)
			return VA_STATUS_ERROR_INVALID_BUFFER;
		if (dd->buf_types[idx] == VAPictureParameterBufferType &&
		    dd->buf_sizes[idx] == sizeof(VADecPictureParameterBufferVP9)) {
			struct vpu_decode_ctx *old = vpu_decode_vp9_predecessor(
				eng, dd->buf_data[idx]);

			for (int j = 0; old && j < dd->n_retired_vp9; j++) {
				if (dd->retired_vp9[j] != old)
					continue;
				/* Only explicit references to a retired context permit
				 * continuation. Active contexts never share sessions.
				 */
				vpu_decode_destroy(eng);
				engine->dec = eng = old;
				dd->retired_vp9[j] = dd->retired_vp9[--dd->n_retired_vp9];
				if (vpu_decode_begin(eng, engine->target))
					return VA_STATUS_ERROR_OPERATION_FAILED;
				DBG("[vp9] resumed reference owner for context %u\n", context_id);
				break;
			}
		}
		ret = vpu_decode_render(eng, dd->buf_types[idx],
			dd->buf_data[idx], dd->buf_sizes[idx],
			dd->buf_num_elements[idx]);
		if (ret)
			return dd->buf_types[idx] == VASliceDataBufferType ?
				VA_STATUS_ERROR_DECODING_ERROR :
				VA_STATUS_ERROR_INVALID_BUFFER;
	}
	return VA_STATUS_SUCCESS;
}

VAStatus
vpu_vaEndPicture(VADriverContextP ctx, VAContextID context_id)
{
	struct vpu_drv_data *dd = ctx->pDriverData;
	struct vpu_decode_ctx *eng;

	if (!dd)
		return VA_STATUS_ERROR_INVALID_CONTEXT;
	eng = vpu_context_engine(dd, context_id);
	if (!eng)
		return VA_STATUS_ERROR_INVALID_CONTEXT;
	return vpu_decode_end(eng) ?
		VA_STATUS_ERROR_DECODING_ERROR : VA_STATUS_SUCCESS;
}

VAStatus
vpu_vaSyncSurface(VADriverContextP ctx, VASurfaceID render_target)
{
	struct vpu_drv_data *dd = ctx->pDriverData;

	if (!dd)
		return VA_STATUS_ERROR_INVALID_SURFACE;
	if (!dd->surfs || !vpu_surfaces_valid(dd->surfs, render_target))
		return VA_STATUS_ERROR_INVALID_SURFACE;
	/* Route through the registry: the picture being synced may belong to
	 * any live engine, and never-queued pool surfaces succeed without
	 * draining anything. */
	{
		int r = vpu_surfaces_sync(dd->surfs, render_target);

		DBG("[sync] surf=%u -> r=%d\n", render_target, r);
		if (!r)
			return VA_STATUS_SUCCESS;
		return r == -ETIMEDOUT ? VA_STATUS_ERROR_TIMEDOUT :
			VA_STATUS_ERROR_DECODING_ERROR;
	}
}

VAStatus
vpu_vaQuerySurfaceStatus(VADriverContextP ctx, VASurfaceID render_target,
			  VASurfaceStatus *status)
{
	struct vpu_drv_data *dd = ctx->pDriverData;

	if (!dd || !status || !dd->surfs ||
	    !vpu_surfaces_valid(dd->surfs, render_target))
		return VA_STATUS_ERROR_INVALID_SURFACE;
	*status = vpu_surfaces_ready(dd->surfs, render_target) ?
		VASurfaceReady : VASurfaceRendering;
	return VA_STATUS_SUCCESS;
}
