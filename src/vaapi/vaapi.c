// SPDX-License-Identifier: GPL-2.0-or-later
/* VA-API frontend for platform stateful VPU decoders.
 *
 * This module owns the per-display driver state, the driver entrypoints and
 * the vtable.  VA object classes are implemented in sibling modules, all
 * operating on struct vpu_drv_data from vaapi_internal.h. */

#include "vaapi_internal.h"

const VAEntrypoint vpu_entrypoints[VPU_NUM_ENTRYPOINTS] = {
	VAEntrypointVLD,
};

static int g_dbg = -1;

int
vpu_va_dbg_enabled(void)
{
	if (g_dbg < 0)
		g_dbg = getenv("VPU_VAAPI_DEBUG") != NULL;
	return g_dbg;
}

struct vpu_config *
vpu_find_config(struct vpu_drv_data *dd, VAConfigID id)
{
	int i;

	for (i = 0; i < dd->n_configs; i++)
		if (dd->configs[i].id == id)
			return &dd->configs[i];
	return NULL;
}

struct vpu_drv_data *
vpu_drv_data(VADriverContextP ctx)
{
	if (!ctx)
		return NULL;
	if (!ctx->pDriverData) {
		ctx->pDriverData = calloc(1, sizeof(struct vpu_drv_data));
		if (!ctx->pDriverData)
			return NULL;
	}
	return ctx->pDriverData;
}

struct vpu_surfaces *
vpu_ensure_surfs(struct vpu_drv_data *dd)
{
	if (!dd->surfs)
		dd->surfs = vpu_surfaces_create();
	return dd->surfs;
}

static VAStatus
vpu_vaTerminate(VADriverContextP ctx)
{
	struct vpu_drv_data *dd = ctx->pDriverData;
	int i;

	if (dd) {
		vpu_free_buffers(dd);
		for (i = 0; i < dd->img_n; i++)
			free(dd->img_data[i]);
		dd->img_n = 0;
		dd->derived_n = 0;
		for (i = 0; i < dd->n_engines; i++)
			vpu_decode_destroy(dd->engines[i].dec);
		dd->n_engines = 0;
		for (i = 0; i < dd->n_retired_vp9; i++)
			vpu_decode_destroy(dd->retired_vp9[i]);
		vpu_surfaces_destroy(dd->surfs);
		dd->surfs = NULL;
		vpu_platform_destroy(dd->platform);
		dd->platform = NULL;
		free(dd);
		ctx->pDriverData = NULL;
	}
	return VA_STATUS_SUCCESS;
}

/* Image/subpicture entries are required non-NULL by libva's validation. */
static VAStatus
vpu_vaQuerySubpictureFormats(VADriverContextP ctx, VAImageFormat *format_list,
			      unsigned int *flags, unsigned int *num_formats)
{
	if (num_formats)
		*num_formats = 0;
	return VA_STATUS_SUCCESS;
}

static VAStatus
vpu_vaCreateSubpicture(VADriverContextP ctx, VAImageID image,
			VASubpictureID *subpicture)
{
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus
vpu_vaDestroySubpicture(VADriverContextP ctx, VASubpictureID subpicture)
{
	return VA_STATUS_SUCCESS;
}

static VAStatus
vpu_vaSetSubpictureImage(VADriverContextP ctx, VASubpictureID subpicture,
			  VAImageID image)
{
	return VA_STATUS_SUCCESS;
}

static VAStatus
vpu_vaSetSubpictureChromakey(VADriverContextP ctx, VASubpictureID subpicture,
			      unsigned int chromakey_min,
			      unsigned int chromakey_max,
			      unsigned int chromakey_mask)
{
	return VA_STATUS_SUCCESS;
}

static VAStatus
vpu_vaSetSubpictureGlobalAlpha(VADriverContextP ctx, VASubpictureID subpicture,
				float global_alpha)
{
	return VA_STATUS_SUCCESS;
}

static VAStatus
vpu_vaAssociateSubpicture(VADriverContextP ctx, VASubpictureID subpicture,
			   VASurfaceID *target_surfaces, int num_surfaces,
			   short src_x, short src_y, unsigned short src_width,
			   unsigned short src_height, short dest_x, short dest_y,
			   unsigned short dest_width, unsigned short dest_height,
			   unsigned int flags)
{
	return VA_STATUS_SUCCESS;
}

static VAStatus
vpu_vaDeassociateSubpicture(VADriverContextP ctx, VASubpictureID subpicture,
			     VASurfaceID *target_surfaces, int num_surfaces)
{
	return VA_STATUS_SUCCESS;
}

static VAStatus
vpu_vaQueryDisplayAttributes(VADriverContextP ctx, VADisplayAttribute *attr_list,
			      int *num_attributes)
{
	if (!num_attributes)
		return VA_STATUS_ERROR_INVALID_PARAMETER;
	*num_attributes = 0;
	return VA_STATUS_SUCCESS;
}

static VAStatus
vpu_vaGetDisplayAttributes(VADriverContextP ctx, VADisplayAttribute *attr_list,
			    int num_attributes)
{
	return VA_STATUS_SUCCESS;
}

static VAStatus
vpu_vaSetDisplayAttributes(VADriverContextP ctx, VADisplayAttribute *attr_list,
			    int num_attributes)
{
	return VA_STATUS_SUCCESS;
}

static const struct VADriverVTable vpu_vtable_template = {
	.vaTerminate = vpu_vaTerminate,
	.vaQueryConfigProfiles = vpu_vaQueryConfigProfiles,
	.vaQueryConfigEntrypoints = vpu_vaQueryConfigEntrypoints,
	.vaGetConfigAttributes = vpu_vaGetConfigAttributes,
	.vaCreateConfig = vpu_vaCreateConfig,
	.vaDestroyConfig = vpu_vaDestroyConfig,
	.vaQueryConfigAttributes = vpu_vaQueryConfigAttributes,
	.vaCreateSurfaces = vpu_vaCreateSurfaces,
	.vaDestroySurfaces = vpu_vaDestroySurfaces,
	.vaCreateContext = vpu_vaCreateContext,
	.vaDestroyContext = vpu_vaDestroyContext,
	.vaCreateBuffer = vpu_vaCreateBuffer,
	.vaBufferSetNumElements = vpu_vaBufferSetNumElements,
	.vaMapBuffer = vpu_vaMapBuffer,
	.vaUnmapBuffer = vpu_vaUnmapBuffer,
	.vaDestroyBuffer = vpu_vaDestroyBuffer,
	.vaBeginPicture = vpu_vaBeginPicture,
	.vaRenderPicture = vpu_vaRenderPicture,
	.vaEndPicture = vpu_vaEndPicture,
	.vaSyncSurface = vpu_vaSyncSurface,
	.vaQuerySurfaceStatus = vpu_vaQuerySurfaceStatus,
	.vaQueryImageFormats = vpu_vaQueryImageFormats,
	.vaCreateImage = vpu_vaCreateImage,
	.vaDeriveImage = vpu_vaDeriveImage,
	.vaDestroyImage = vpu_vaDestroyImage,
	.vaSetImagePalette = vpu_vaSetImagePalette,
	.vaGetImage = vpu_vaGetImage,
	.vaPutImage = vpu_vaPutImage,
	.vaQuerySubpictureFormats = vpu_vaQuerySubpictureFormats,
	.vaCreateSubpicture = vpu_vaCreateSubpicture,
	.vaDestroySubpicture = vpu_vaDestroySubpicture,
	.vaSetSubpictureImage = vpu_vaSetSubpictureImage,
	.vaSetSubpictureChromakey = vpu_vaSetSubpictureChromakey,
	.vaSetSubpictureGlobalAlpha = vpu_vaSetSubpictureGlobalAlpha,
	.vaAssociateSubpicture = vpu_vaAssociateSubpicture,
	.vaDeassociateSubpicture = vpu_vaDeassociateSubpicture,
	.vaQueryDisplayAttributes = vpu_vaQueryDisplayAttributes,
	.vaGetDisplayAttributes = vpu_vaGetDisplayAttributes,
	.vaSetDisplayAttributes = vpu_vaSetDisplayAttributes,
	.vaBufferInfo = vpu_vaBufferInfo,
	.vaCreateSurfaces2 = vpu_vaCreateSurfaces2,
	.vaQuerySurfaceAttributes = vpu_vaQuerySurfaceAttributes,
	.vaExportSurfaceHandle = vpu_vaExportSurfaceHandle,
};

VPU_PUBLIC VAStatus
__vaDriverInit_1_23(VADriverContextP ctx, int major_version, int minor_version)
{
	struct VADriverVTable *vt;

	if (!ctx)
		return VA_STATUS_ERROR_INVALID_PARAMETER;

	/*
	 * libva owns ctx->vtable after init and frees it in vaTerminate(), so
	 * hand it a heap copy of the static template rather than the template
	 * itself (which lives in .rodata and cannot be freed).
	 */
	vt = calloc(1, sizeof(*vt));
	if (!vt)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	*vt = vpu_vtable_template;

	ctx->version_major = major_version;
	ctx->version_minor = minor_version;
	ctx->vtable = vt;
	ctx->max_profiles = vpu_codec_profile_count();
	ctx->max_entrypoints = VPU_NUM_ENTRYPOINTS;
	ctx->max_attributes = 1;
	ctx->max_image_formats = 2;
	ctx->max_subpic_formats = 1;
	ctx->max_display_attributes = 0;
	ctx->str_vendor = "vpu-vaapi " VPU_VAAPI_VERSION;
	ctx->pDriverData = NULL;

	{
		struct vpu_drv_data *dd = vpu_drv_data(ctx);

		if (!dd)
			return VA_STATUS_ERROR_ALLOCATION_FAILED;
		dd->platform = vpu_platform_create(NULL, NULL);
		if (!dd->platform) {
			free(dd);
			ctx->pDriverData = NULL;
			return VA_STATUS_ERROR_OPERATION_FAILED;
		}
		snprintf(dd->vendor, sizeof(dd->vendor),
			 "vpu-vaapi: %s (%s) %s",
			 vpu_platform_description(dd->platform),
			 vpu_platform_name(dd->platform), VPU_VAAPI_VERSION);
		ctx->str_vendor = dd->vendor;
		for (unsigned int i = 0; i < vpu_codec_profile_count(); i++) {
			VAProfile profile = vpu_codec_profile_at(i);
			enum vpu_pixel_format format;

			if (!vpu_codec_profile_info(profile, NULL, &format, NULL) &&
			    format == VPU_PIXEL_FORMAT_P010 &&
			    vpu_profile_supported(dd, profile)) {
				dd->p010_supported = 1;
				break;
			}
		}
		if (!dd->p010_supported)
			fprintf(stderr,
				"vpu-vaapi: platform %s on %s does not advertise P010 profiles\n",
				vpu_platform_name(dd->platform),
				vpu_platform_device(dd->platform));
	}

	return VA_STATUS_SUCCESS;
}

/* Compatibility aliases for older libva releases */
VPU_PUBLIC VAStatus
vaDriverInit(VADriverContextP ctx, int major_version, int minor_version)
{
	return __vaDriverInit_1_23(ctx, major_version, minor_version);
}
