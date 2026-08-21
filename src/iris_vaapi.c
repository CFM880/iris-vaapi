// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * iris-vaapi: P0 VA-API driver skeleton for the Qualcomm Iris (SM8150)
 * stateful V4L2 decoder.
 *
 * This stage only registers the driver with libva and advertises the codec
 * profiles/entrypoints so that `vainfo` (and eventually Chrome) can see the
 * hardware.  Decode is intentionally not wired up yet.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <va/va.h>
#include <va/va_backend.h>

#define IRIS_VAAPI_VERSION	"0.0.1"
#define IRIS_VAAPI_VENDOR	"iris-vaapi P0: Qualcomm Iris SM8150 (V4L2) "

/* vtable needs a little state to hand out ids */
struct iris_drv_data {
	unsigned int config_id;
	unsigned int context_id;
	unsigned int surface_id;
	unsigned int buffer_id;
};

static const VAProfile iris_profiles[] = {
	VAProfileH264ConstrainedBaseline,
	VAProfileH264Main,
	VAProfileH264High,
	VAProfileHEVCMain,
	VAProfileVP9Profile0,
};
#define NUM_IRIS_PROFILES (sizeof(iris_profiles) / sizeof(iris_profiles[0]))

static const VAEntrypoint iris_entrypoints[] = {
	VAEntrypointVLD,
};
#define NUM_IRIS_ENTRYPOINTS (sizeof(iris_entrypoints) / sizeof(iris_entrypoints[0]))

static struct iris_drv_data *
iris_drv_data(VADriverContextP ctx)
{
	if (!ctx)
		return NULL;
	if (!ctx->pDriverData) {
		ctx->pDriverData = calloc(1, sizeof(struct iris_drv_data));
		if (!ctx->pDriverData)
			return NULL;
	}
	return ctx->pDriverData;
}

static VAStatus
iris_vaTerminate(VADriverContextP ctx)
{
	free(ctx->pDriverData);
	ctx->pDriverData = NULL;
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaQueryConfigProfiles(VADriverContextP ctx, VAProfile *profile_list,
			   int *num_profiles)
{
	if (!num_profiles)
		return VA_STATUS_ERROR_INVALID_PARAMETER;

	if (profile_list && *num_profiles >= (int)NUM_IRIS_PROFILES)
		memcpy(profile_list, iris_profiles,
		       NUM_IRIS_PROFILES * sizeof(*profile_list));
	else if (!profile_list)
		*num_profiles = NUM_IRIS_PROFILES;
	else if (*num_profiles < (int)NUM_IRIS_PROFILES)
		return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;

	*num_profiles = NUM_IRIS_PROFILES;
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaQueryConfigEntrypoints(VADriverContextP ctx, VAProfile profile,
			      VAEntrypoint *entrypoint_list, int *num_entrypoints)
{
	int i;

	if (!num_entrypoints)
		return VA_STATUS_ERROR_INVALID_PARAMETER;

	for (i = 0; i < (int)NUM_IRIS_PROFILES; i++) {
		if (iris_profiles[i] == profile)
			break;
	}
	if (i == (int)NUM_IRIS_PROFILES) {
		*num_entrypoints = 0;
		return VA_STATUS_SUCCESS;
	}

	if (entrypoint_list && *num_entrypoints >= (int)NUM_IRIS_ENTRYPOINTS)
		memcpy(entrypoint_list, iris_entrypoints,
		       NUM_IRIS_ENTRYPOINTS * sizeof(*entrypoint_list));
	else if (!entrypoint_list)
		*num_entrypoints = NUM_IRIS_ENTRYPOINTS;
	else if (*num_entrypoints < (int)NUM_IRIS_ENTRYPOINTS)
		return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;

	*num_entrypoints = NUM_IRIS_ENTRYPOINTS;
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaGetConfigAttributes(VADriverContextP ctx, VAProfile profile,
			   VAEntrypoint entrypoint, VAConfigAttrib *attrib_list,
			   int num_attribs)
{
	int i;

	for (i = 0; i < num_attribs; i++) {
		switch (attrib_list[i].type) {
		case VAConfigAttribRTFormat:
			attrib_list[i].value =
				VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_YUV420_10BPP;
			break;
		default:
			attrib_list[i].value = VA_ATTRIB_NOT_SUPPORTED;
			break;
		}
	}
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaCreateConfig(VADriverContextP ctx, VAProfile profile,
		    VAEntrypoint entrypoint, VAConfigAttrib *attrib_list,
		    int num_attribs, VAConfigID *config_id)
{
	struct iris_drv_data *dd;

	if (entrypoint != VAEntrypointVLD)
		return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;

	dd = iris_drv_data(ctx);
	if (!dd)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;

	*config_id = ++dd->config_id;
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaDestroyConfig(VADriverContextP ctx, VAConfigID config_id)
{
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaQueryConfigAttributes(VADriverContextP ctx, VAConfigID config_id,
			     VAProfile *profile, VAEntrypoint *entrypoint,
			     VAConfigAttrib *attrib_list, int *num_attribs)
{
	*profile = VAProfileH264High;
	*entrypoint = VAEntrypointVLD;
	if (attrib_list && num_attribs && *num_attribs) {
		attrib_list[0].type = VAConfigAttribRTFormat;
		attrib_list[0].value = VA_RT_FORMAT_YUV420;
		*num_attribs = 1;
	} else if (num_attribs) {
		*num_attribs = 1;
	}
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaCreateContext(VADriverContextP ctx, VAConfigID config_id, int picture_width,
		     int picture_height, int flag, VASurfaceID *render_targets,
		     int num_render_targets, VAContextID *context_id)
{
	struct iris_drv_data *dd;

	dd = iris_drv_data(ctx);
	if (!dd)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;

	*context_id = ++dd->context_id;
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaDestroyContext(VADriverContextP ctx, VAContextID context_id)
{
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaCreateSurfaces(VADriverContextP ctx, int width, int height, int format,
		      int num_surfaces, VASurfaceID *surfaces)
{
	struct iris_drv_data *dd;
	int i;

	dd = iris_drv_data(ctx);
	if (!dd)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;

	for (i = 0; i < num_surfaces; i++)
		surfaces[i] = ++dd->surface_id;

	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaDestroySurfaces(VADriverContextP ctx, VASurfaceID *surface_list,
		       int num_surfaces)
{
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaCreateSurfaces2(VADriverContextP ctx, unsigned int format,
		       unsigned int width, unsigned int height,
		       VASurfaceID *surfaces, unsigned int num_surfaces,
		       VASurfaceAttrib *attrib_list, unsigned int num_attribs)
{
	return iris_vaCreateSurfaces(ctx, width, height, format, num_surfaces,
				     surfaces);
}

static VAStatus
iris_vaCreateBuffer(VADriverContextP ctx, VAContextID context_id,
		    VABufferType type, unsigned int size, unsigned int num_elements,
		    void *data, VABufferID *buf_id)
{
	struct iris_drv_data *dd;

	dd = iris_drv_data(ctx);
	if (!dd)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;

	*buf_id = ++dd->buffer_id;
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaBufferSetNumElements(VADriverContextP ctx, VABufferID buf_id,
			    unsigned int num_elements)
{
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaMapBuffer(VADriverContextP ctx, VABufferID buf_id, void **pbuf)
{
	*pbuf = NULL;
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus
iris_vaUnmapBuffer(VADriverContextP ctx, VABufferID buf_id)
{
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaDestroyBuffer(VADriverContextP ctx, VABufferID buf_id)
{
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaBeginPicture(VADriverContextP ctx, VAContextID context_id,
		    VASurfaceID render_target)
{
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus
iris_vaRenderPicture(VADriverContextP ctx, VAContextID context_id,
		     VABufferID *buffers, int num_buffers)
{
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus
iris_vaEndPicture(VADriverContextP ctx, VAContextID context_id)
{
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus
iris_vaSyncSurface(VADriverContextP ctx, VASurfaceID render_target)
{
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus
iris_vaQuerySurfaceStatus(VADriverContextP ctx, VASurfaceID render_target,
			  VASurfaceStatus *status)
{
	*status = VASurfaceReady;
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaQueryImageFormats(VADriverContextP ctx, VAImageFormat *format_list,
			 int *num_formats)
{
	*num_formats = 0;
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaQueryDisplayAttributes(VADriverContextP ctx, VADisplayAttribute *attr_list,
			      int *num_attributes)
{
	if (!num_attributes)
		return VA_STATUS_ERROR_INVALID_PARAMETER;
	*num_attributes = 0;
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaGetDisplayAttributes(VADriverContextP ctx, VADisplayAttribute *attr_list,
			    int num_attributes)
{
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaQuerySurfaceAttributes(VADriverContextP dpy, VAConfigID config,
			      VASurfaceAttrib *attrib_list,
			      unsigned int *num_attribs)
{
	if (attrib_list && num_attribs && *num_attribs) {
		attrib_list[0].type = VASurfaceAttribPixelFormat;
		attrib_list[0].value.value.i = VA_FOURCC_NV12;
		*num_attribs = 1;
	} else if (num_attribs) {
		*num_attribs = 1;
	}
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaBufferInfo(VADriverContextP ctx, VABufferID buf_id,
		  VABufferType *type, unsigned int *size, unsigned int *num_elements)
{
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

/* Image/subpicture entries are required non-NULL by libva's driver
 * validation even for a decode-only P0 driver. */
static VAStatus
iris_vaCreateImage(VADriverContextP ctx, VAImageFormat *format, int width,
		   int height, VAImage *image)
{
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus
iris_vaDeriveImage(VADriverContextP ctx, VASurfaceID surface, VAImage *image)
{
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus
iris_vaDestroyImage(VADriverContextP ctx, VAImageID image)
{
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaSetImagePalette(VADriverContextP ctx, VAImageID image,
		       unsigned char *palette)
{
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaGetImage(VADriverContextP ctx, VASurfaceID surface, int x, int y,
		unsigned int width, unsigned int height, VAImageID image)
{
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus
iris_vaPutImage(VADriverContextP ctx, VASurfaceID surface, VAImageID image,
		int src_x, int src_y, unsigned int src_width,
		unsigned int src_height, int dest_x, int dest_y,
		unsigned int dest_width, unsigned int dest_height)
{
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus
iris_vaQuerySubpictureFormats(VADriverContextP ctx, VAImageFormat *format_list,
			      unsigned int *flags, unsigned int *num_formats)
{
	if (num_formats)
		*num_formats = 0;
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaCreateSubpicture(VADriverContextP ctx, VAImageID image,
			VASubpictureID *subpicture)
{
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus
iris_vaDestroySubpicture(VADriverContextP ctx, VASubpictureID subpicture)
{
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaSetSubpictureImage(VADriverContextP ctx, VASubpictureID subpicture,
			  VAImageID image)
{
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaSetSubpictureChromakey(VADriverContextP ctx, VASubpictureID subpicture,
			      unsigned int chromakey_min,
			      unsigned int chromakey_max,
			      unsigned int chromakey_mask)
{
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaSetSubpictureGlobalAlpha(VADriverContextP ctx, VASubpictureID subpicture,
				float global_alpha)
{
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaAssociateSubpicture(VADriverContextP ctx, VASubpictureID subpicture,
			   VASurfaceID *target_surfaces, int num_surfaces,
			   short src_x, short src_y, unsigned short src_width,
			   unsigned short src_height, short dest_x, short dest_y,
			   unsigned short dest_width, unsigned short dest_height,
			   unsigned int flags)
{
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaDeassociateSubpicture(VADriverContextP ctx, VASubpictureID subpicture,
			     VASurfaceID *target_surfaces, int num_surfaces)
{
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaSetDisplayAttributes(VADriverContextP ctx, VADisplayAttribute *attr_list,
			    int num_attributes)
{
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaExportSurfaceHandle(VADriverContextP ctx, VASurfaceID surface_id,
			   uint32_t mem_type, uint32_t flags, void *descriptor)
{
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static const struct VADriverVTable iris_vtable_template = {
	.vaTerminate = iris_vaTerminate,
	.vaQueryConfigProfiles = iris_vaQueryConfigProfiles,
	.vaQueryConfigEntrypoints = iris_vaQueryConfigEntrypoints,
	.vaGetConfigAttributes = iris_vaGetConfigAttributes,
	.vaCreateConfig = iris_vaCreateConfig,
	.vaDestroyConfig = iris_vaDestroyConfig,
	.vaQueryConfigAttributes = iris_vaQueryConfigAttributes,
	.vaCreateSurfaces = iris_vaCreateSurfaces,
	.vaDestroySurfaces = iris_vaDestroySurfaces,
	.vaCreateContext = iris_vaCreateContext,
	.vaDestroyContext = iris_vaDestroyContext,
	.vaCreateBuffer = iris_vaCreateBuffer,
	.vaBufferSetNumElements = iris_vaBufferSetNumElements,
	.vaMapBuffer = iris_vaMapBuffer,
	.vaUnmapBuffer = iris_vaUnmapBuffer,
	.vaDestroyBuffer = iris_vaDestroyBuffer,
	.vaBeginPicture = iris_vaBeginPicture,
	.vaRenderPicture = iris_vaRenderPicture,
	.vaEndPicture = iris_vaEndPicture,
	.vaSyncSurface = iris_vaSyncSurface,
	.vaQuerySurfaceStatus = iris_vaQuerySurfaceStatus,
	.vaQueryImageFormats = iris_vaQueryImageFormats,
	.vaCreateImage = iris_vaCreateImage,
	.vaDeriveImage = iris_vaDeriveImage,
	.vaDestroyImage = iris_vaDestroyImage,
	.vaSetImagePalette = iris_vaSetImagePalette,
	.vaGetImage = iris_vaGetImage,
	.vaPutImage = iris_vaPutImage,
	.vaQuerySubpictureFormats = iris_vaQuerySubpictureFormats,
	.vaCreateSubpicture = iris_vaCreateSubpicture,
	.vaDestroySubpicture = iris_vaDestroySubpicture,
	.vaSetSubpictureImage = iris_vaSetSubpictureImage,
	.vaSetSubpictureChromakey = iris_vaSetSubpictureChromakey,
	.vaSetSubpictureGlobalAlpha = iris_vaSetSubpictureGlobalAlpha,
	.vaAssociateSubpicture = iris_vaAssociateSubpicture,
	.vaDeassociateSubpicture = iris_vaDeassociateSubpicture,
	.vaQueryDisplayAttributes = iris_vaQueryDisplayAttributes,
	.vaGetDisplayAttributes = iris_vaGetDisplayAttributes,
	.vaSetDisplayAttributes = iris_vaSetDisplayAttributes,
	.vaBufferInfo = iris_vaBufferInfo,
	.vaCreateSurfaces2 = iris_vaCreateSurfaces2,
	.vaQuerySurfaceAttributes = iris_vaQuerySurfaceAttributes,
	.vaExportSurfaceHandle = iris_vaExportSurfaceHandle,
};

VAStatus
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
	*vt = iris_vtable_template;

	ctx->version_major = major_version;
	ctx->version_minor = minor_version;
	ctx->vtable = vt;
	ctx->max_profiles = NUM_IRIS_PROFILES;
	ctx->max_entrypoints = NUM_IRIS_ENTRYPOINTS;
	ctx->max_attributes = 1;
	ctx->max_image_formats = 1;
	ctx->max_subpic_formats = 1;
	ctx->max_display_attributes = 0;
	ctx->str_vendor = IRIS_VAAPI_VENDOR IRIS_VAAPI_VERSION;
	ctx->pDriverData = NULL;

	if (!iris_drv_data(ctx))
		return VA_STATUS_ERROR_ALLOCATION_FAILED;

	return VA_STATUS_SUCCESS;
}

/* Compatibility aliases for older libva releases */
VAStatus
vaDriverInit(VADriverContextP ctx, int major_version, int minor_version)
{
	return __vaDriverInit_1_23(ctx, major_version, minor_version);
}