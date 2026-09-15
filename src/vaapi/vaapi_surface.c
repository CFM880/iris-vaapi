// SPDX-License-Identifier: GPL-2.0-or-later
/* VA surface objects: pool allocation into the display-level registry, the
 * opt-in direct CAPTURE spare pool, surface attribute queries and DRM PRIME
 * export. */

#include "vaapi_internal.h"

VAStatus
vpu_vaCreateSurfaces(VADriverContextP ctx, int width, int height, int format,
		      int num_surfaces, VASurfaceID *surfaces)
{
	struct vpu_drv_data *dd;
	struct vpu_surfaces *t;
	unsigned int fourcc;
	int i = 0;

	dd = vpu_drv_data(ctx);
	if (!dd)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	t = vpu_ensure_surfs(dd);
	if (!t)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	if ((format & VA_RT_FORMAT_YUV420_10) && !dd->p010_supported)
		return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
	fourcc = (format & VA_RT_FORMAT_YUV420_10) ? VA_FOURCC_P010 :
		VA_FOURCC_NV12;

	while (i < num_surfaces && dd->direct_spare_count &&
	       dd->direct_spare_width == (unsigned int)width &&
	       dd->direct_spare_height == (unsigned int)height &&
	       dd->direct_spare_fourcc == fourcc) {
		surfaces[i++] = dd->direct_spares[0];
		memmove(dd->direct_spares, dd->direct_spares + 1,
			(--dd->direct_spare_count) * sizeof(dd->direct_spares[0]));
	}

	if (i < num_surfaces && getenv("VPU_DIRECT_CAPTURE") &&
	    !dd->direct_pool_initialized) {
		const char *slots_env = getenv("VPU_DIRECT_CAPTURE_SLOTS");
		int requested = num_surfaces - i;
		int allocate = requested < 20 ? 20 : requested;
		int j;

		if (slots_env && *slots_env) {
			char *end;
			unsigned long slots = strtoul(slots_env, &end, 10);

			if (!*end && slots > (unsigned int)allocate &&
			    slots <= ARRAY_SIZE(dd->direct_spares))
				allocate = (int)slots;
		}

		dd->direct_pool_initialized = 1;
		dd->direct_spare_width = width;
		dd->direct_spare_height = height;
		dd->direct_spare_fourcc = fourcc;
		for (j = 0; j < allocate; j++) {
			VASurfaceID id = ++dd->surface_id;

			if (vpu_surfaces_alloc(t, id, width, height, fourcc))
				return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
			if (j < requested)
				surfaces[i++] = id;
			else if (dd->direct_spare_count <
				 ARRAY_SIZE(dd->direct_spares))
				dd->direct_spares[dd->direct_spare_count++] = id;
		}
	}

	for (; i < num_surfaces; i++) {
		VASurfaceID id = ++dd->surface_id;

		if (vpu_surfaces_alloc(t, id, width, height, fourcc))
			return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
		surfaces[i] = id;
	}

	return VA_STATUS_SUCCESS;
}

VAStatus
vpu_vaDestroySurfaces(VADriverContextP ctx, VASurfaceID *surface_list,
		       int num_surfaces)
{
	struct vpu_drv_data *dd = ctx->pDriverData;
	int i;

	if (!dd || !dd->surfs || !surface_list || num_surfaces < 0)
		return VA_STATUS_ERROR_INVALID_PARAMETER;
	for (i = 0; i < num_surfaces; i++) {
		void *surface_mem;
		unsigned int pitch, size, width, height, fourcc;
		int j = 0;

		if (vpu_surfaces_peek_buffer(dd->surfs, surface_list[i], &surface_mem,
				      &pitch, &size, &width, &height, &fourcc))
			return VA_STATUS_ERROR_INVALID_SURFACE;
		/* A client should destroy derived images first.  Invalidate any that
		 * remain so vaMapBuffer can never return a dangling surface mapping. */
		while (j < dd->derived_n) {
			if (dd->derived_mem[j] != surface_mem) {
				j++;
				continue;
			}
			dd->derived_ids[j] = dd->derived_ids[dd->derived_n - 1];
			dd->derived_mem[j] = dd->derived_mem[dd->derived_n - 1];
			dd->derived_n--;
		}
		vpu_surfaces_free(dd->surfs, surface_list[i]);
	}
	for (i = 0; i < dd->n_retired_vp9;) {
		if (vpu_decode_retain_vp9(dd->retired_vp9[i])) {
			i++;
			continue;
		}
		vpu_decode_destroy(dd->retired_vp9[i]);
		dd->retired_vp9[i] = dd->retired_vp9[--dd->n_retired_vp9];
	}
	return VA_STATUS_SUCCESS;
}

VAStatus
vpu_vaCreateSurfaces2(VADriverContextP ctx, unsigned int format,
		       unsigned int width, unsigned int height,
		       VASurfaceID *surfaces, unsigned int num_surfaces,
		       VASurfaceAttrib *attrib_list, unsigned int num_attribs)
{
	unsigned int i;

	for (i = 0; i < num_attribs; i++)
		if (attrib_list[i].type == VASurfaceAttribPixelFormat &&
		    attrib_list[i].value.type == VAGenericValueTypeInteger &&
		    attrib_list[i].value.value.i == VA_FOURCC_P010)
			format = VA_RT_FORMAT_YUV420_10;
	return vpu_vaCreateSurfaces(ctx, width, height, format, num_surfaces,
				     surfaces);
}

VAStatus
vpu_vaQuerySurfaceAttributes(VADriverContextP dpy, VAConfigID config,
			      VASurfaceAttrib *attrib_list,
			      unsigned int *num_attribs)
{
	struct vpu_drv_data *dd = dpy ? dpy->pDriverData : NULL;
	struct vpu_config *cfg;
	VASurfaceAttrib attrs[] = {
		{ .type = VASurfaceAttribPixelFormat,
		  .flags = VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE,
		  .value.type = VAGenericValueTypeInteger,
		  .value.value.i = VA_FOURCC_NV12 },
		{ .type = VASurfaceAttribMinWidth,
		  .flags = VA_SURFACE_ATTRIB_GETTABLE,
		  .value.type = VAGenericValueTypeInteger, .value.value.i = 16 },
		{ .type = VASurfaceAttribMaxWidth,
		  .flags = VA_SURFACE_ATTRIB_GETTABLE,
		  .value.type = VAGenericValueTypeInteger, .value.value.i = 4096 },
		{ .type = VASurfaceAttribMinHeight,
		  .flags = VA_SURFACE_ATTRIB_GETTABLE,
		  .value.type = VAGenericValueTypeInteger, .value.value.i = 16 },
		{ .type = VASurfaceAttribMaxHeight,
		  .flags = VA_SURFACE_ATTRIB_GETTABLE,
		  .value.type = VAGenericValueTypeInteger, .value.value.i = 4096 },
	};
	unsigned int want = ARRAY_SIZE(attrs);

	if (!num_attribs)
		return VA_STATUS_ERROR_INVALID_PARAMETER;
	if (!dd)
		return VA_STATUS_ERROR_INVALID_CONFIG;
	cfg = vpu_find_config(dd, config);
	if (!cfg)
		return VA_STATUS_ERROR_INVALID_CONFIG;
	attrs[0].value.value.i = vpu_profile_fourcc(cfg->profile);
	if (attrib_list) {
		if (*num_attribs < want) {
			*num_attribs = want;
			return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
		}
		memcpy(attrib_list, attrs, sizeof(attrs));
		*num_attribs = want;
	} else {
		*num_attribs = want;
	}
	return VA_STATUS_SUCCESS;
}

VAStatus
vpu_vaExportSurfaceHandle(VADriverContextP ctx, VASurfaceID surface_id,
			   uint32_t mem_type, uint32_t flags, void *descriptor)
{
	struct vpu_drv_data *dd = ctx->pDriverData;
	VADRMPRIMESurfaceDescriptor *d = descriptor;
	unsigned int w, h, pitch, size, fourcc;
	uint32_t layer_flags;
	int fd;

	if (mem_type != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2)
		return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
	if (!dd || !dd->surfs)
		return VA_STATUS_ERROR_INVALID_SURFACE;
	if (!descriptor)
		return VA_STATUS_ERROR_INVALID_PARAMETER;
	layer_flags = flags & (VA_EXPORT_SURFACE_SEPARATE_LAYERS |
			       VA_EXPORT_SURFACE_COMPOSED_LAYERS);
	if (layer_flags != VA_EXPORT_SURFACE_SEPARATE_LAYERS &&
	    layer_flags != VA_EXPORT_SURFACE_COMPOSED_LAYERS)
		return VA_STATUS_ERROR_INVALID_PARAMETER;

	DBG("[export] surf=%u type=%u flags=0x%x\n", surface_id,
	    mem_type, flags);
	if (vpu_surfaces_export(dd->surfs, surface_id, &fd, &pitch, &size,
			      &w, &h, &fourcc))
		return VA_STATUS_ERROR_INVALID_SURFACE;

	memset(d, 0, sizeof(*d));
	d->fourcc = fourcc;
	d->width = w;
	d->height = h;
	d->num_objects = 1;
	d->objects[0].fd = fd;
	d->objects[0].size = size;
	d->objects[0].drm_format_modifier = DRM_FORMAT_MOD_LINEAR;
	if (layer_flags == VA_EXPORT_SURFACE_COMPOSED_LAYERS) {
		d->num_layers = 1;
		d->layers[0].drm_format = fourcc == VA_FOURCC_P010 ?
			DRM_FORMAT_P010 : DRM_FORMAT_NV12;
		d->layers[0].num_planes = 2;
		d->layers[0].object_index[0] = 0;
		d->layers[0].object_index[1] = 0;
		d->layers[0].offset[0] = 0;
		d->layers[0].offset[1] = pitch * h;
		d->layers[0].pitch[0] = pitch;
		d->layers[0].pitch[1] = pitch;
	} else {
		/* Chrome requests separate layers and requires one plane per layer. */
		d->num_layers = 2;
		d->layers[0].drm_format = fourcc == VA_FOURCC_P010 ?
			DRM_FORMAT_R16 : DRM_FORMAT_R8;
		d->layers[0].num_planes = 1;
		d->layers[0].object_index[0] = 0;
		d->layers[0].offset[0] = 0;
		d->layers[0].pitch[0] = pitch;
		d->layers[1].drm_format = fourcc == VA_FOURCC_P010 ?
			DRM_FORMAT_GR1616 : DRM_FORMAT_GR88;
		d->layers[1].num_planes = 1;
		d->layers[1].object_index[0] = 0;
		d->layers[1].offset[0] = pitch * h;
		d->layers[1].pitch[0] = pitch;
	}
	return VA_STATUS_SUCCESS;
}
