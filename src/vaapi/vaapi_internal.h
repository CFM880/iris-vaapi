// SPDX-License-Identifier: GPL-2.0-or-later
/* Shared state and helper contracts for the VA-API frontend modules.
 *
 * The frontend is split by VA object class; every module operates on the
 * per-display struct vpu_drv_data defined here. */

#ifndef VPU_VAAPI_INTERNAL_H
#define VPU_VAAPI_INTERNAL_H

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <va/va.h>
#include <va/va_backend.h>
#include <va/va_drmcommon.h>

#include "decode/decode.h"
#include "platform/platform.h"
#include "codec/codec.h"

#ifndef ALIGN
#define ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))
#endif
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

#ifndef DRM_FORMAT_NV12
#define DRM_FORMAT_NV12	0x3231564e	/* NV12 fourcc */
#endif
#ifndef DRM_FORMAT_P010
#define DRM_FORMAT_P010	0x30313050	/* P010 fourcc */
#endif
#ifndef DRM_FORMAT_MOD_LINEAR
#define DRM_FORMAT_MOD_LINEAR	0
#endif
#ifndef DRM_FORMAT_R8
#define DRM_FORMAT_R8		0x20203852	/* 'R' '8' 0x20 */
#endif
#ifndef DRM_FORMAT_GR88
#define DRM_FORMAT_GR88		0x38385247	/* 'GR88' */
#endif
#ifndef DRM_FORMAT_R16
#define DRM_FORMAT_R16		0x20363152	/* 'R16 ' */
#endif
#ifndef DRM_FORMAT_GR1616
#define DRM_FORMAT_GR1616	0x32335247	/* 'GR32' */
#endif

#define VPU_VAAPI_VERSION	"0.2.0"
#define VPU_PUBLIC	__attribute__((visibility("default")))

/* vtable needs a little state to hand out ids */
#define VPU_MAX_ENGINES	8
#define VPU_MAX_CONFIGS	32
#define VPU_NUM_ENTRYPOINTS	1

/* Per-surface tracing is chatty and the GPU process inherits this stderr;
 * opt in with VPU_VAAPI_DEBUG=1. */
int vpu_va_dbg_enabled(void);

#define DBG(...)	do { if (vpu_va_dbg_enabled()) fprintf(stderr, __VA_ARGS__); } while (0)

struct vpu_config {
	VAConfigID id;
	VAProfile profile;
};

struct vpu_engine {
	VAContextID ctx_id;
	struct vpu_decode_ctx *dec;
	VASurfaceID target;
};

struct vpu_drv_data {
	unsigned int config_id;
	unsigned int context_id;
	unsigned int surface_id;
	unsigned int buffer_id;
	unsigned int width, height;
	int p010_supported;
	struct vpu_platform *platform;
	char vendor[192];
	struct vpu_config configs[VPU_MAX_CONFIGS];
	int n_configs;
	/* Display-level surface registry: pool surfaces outlive the contexts
	 * that decode into them (Chrome destroys contexts on navigation while
	 * frames are still exported/displayed). */
	struct vpu_surfaces *surfs;
	/* One engine (one V4L2 session) per VA context so concurrent videos
	 * never share firmware DPB/queue state. */
	struct vpu_engine engines[VPU_MAX_ENGINES];
	int n_engines;
	struct vpu_decode_ctx *retired_vp9[VPU_MAX_ENGINES];
	int n_retired_vp9;
	int n_bufs;
	unsigned int buf_ids[256];
	VABufferType buf_types[256];
	unsigned int buf_sizes[256];
	unsigned int buf_num_elements[256];
	void *buf_data[256];
	VASurfaceID direct_spares[32];
	unsigned int direct_spare_count;
	unsigned int direct_spare_width;
	unsigned int direct_spare_height;
	unsigned int direct_spare_fourcc;
	int direct_pool_initialized;

	/* Derived images (vaDeriveImage) map a VABufferID to a CAPTURE buffer. */
	VABufferID derived_ids[256];
	void *derived_mem[256];
	int derived_n;

	/* vaCreateImage/vaGetImage CPU transfer buffers. */
	VABufferID img_ids[64];
	unsigned int img_sizes[64];
	unsigned int img_ws[64];
	unsigned int img_hs[64];
	unsigned int img_fourcc[64];
	void *img_data[64];
	int img_n;
};

extern const VAEntrypoint vpu_entrypoints[VPU_NUM_ENTRYPOINTS];

/* ---- Per-display state (vaapi.c) ---- */
struct vpu_drv_data *vpu_drv_data(VADriverContextP ctx);
struct vpu_config *vpu_find_config(struct vpu_drv_data *dd, VAConfigID id);
struct vpu_surfaces *vpu_ensure_surfs(struct vpu_drv_data *dd);

/* ---- Profile and config queries (vaapi_config.c) ---- */
int vpu_profile_is_10bit(VAProfile profile);
unsigned int vpu_profile_fourcc(VAProfile profile);
unsigned int vpu_profile_rt_format(VAProfile profile);
int vpu_profile_known(VAProfile profile);
enum vpu_codec_id vpu_profile_codec(VAProfile profile);
int vpu_profile_supported(struct vpu_drv_data *dd, VAProfile profile);

/* ---- Buffer table (vaapi_buffer.c) ---- */
void vpu_free_buffers(struct vpu_drv_data *dd);
int vpu_find_buffer(struct vpu_drv_data *dd, VABufferID buf_id);

/* ---- VA entrypoints ---- */
VAStatus vpu_vaQueryConfigProfiles(VADriverContextP ctx, VAProfile *profile_list,
				   int *num_profiles);
VAStatus vpu_vaQueryConfigEntrypoints(VADriverContextP ctx, VAProfile profile,
				      VAEntrypoint *entrypoint_list,
				      int *num_entrypoints);
VAStatus vpu_vaGetConfigAttributes(VADriverContextP ctx, VAProfile profile,
				   VAEntrypoint entrypoint,
				   VAConfigAttrib *attrib_list, int num_attribs);
VAStatus vpu_vaCreateConfig(VADriverContextP ctx, VAProfile profile,
			    VAEntrypoint entrypoint, VAConfigAttrib *attrib_list,
			    int num_attribs, VAConfigID *config_id);
VAStatus vpu_vaDestroyConfig(VADriverContextP ctx, VAConfigID config_id);
VAStatus vpu_vaQueryConfigAttributes(VADriverContextP ctx, VAConfigID config_id,
				     VAProfile *profile, VAEntrypoint *entrypoint,
				     VAConfigAttrib *attrib_list, int *num_attribs);
VAStatus vpu_vaCreateSurfaces(VADriverContextP ctx, int width, int height,
			      int format, int num_surfaces, VASurfaceID *surfaces);
VAStatus vpu_vaDestroySurfaces(VADriverContextP ctx, VASurfaceID *surface_list,
			       int num_surfaces);
VAStatus vpu_vaCreateSurfaces2(VADriverContextP ctx, unsigned int format,
			       unsigned int width, unsigned int height,
			       VASurfaceID *surfaces, unsigned int num_surfaces,
			       VASurfaceAttrib *attrib_list, unsigned int num_attribs);
VAStatus vpu_vaQuerySurfaceAttributes(VADriverContextP dpy, VAConfigID config,
				      VASurfaceAttrib *attrib_list,
				      unsigned int *num_attribs);
VAStatus vpu_vaExportSurfaceHandle(VADriverContextP ctx, VASurfaceID surface_id,
				   uint32_t mem_type, uint32_t flags,
				   void *descriptor);
VAStatus vpu_vaCreateContext(VADriverContextP ctx, VAConfigID config_id,
			     int picture_width, int picture_height, int flag,
			     VASurfaceID *render_targets, int num_render_targets,
			     VAContextID *context_id);
VAStatus vpu_vaDestroyContext(VADriverContextP ctx, VAContextID context_id);
VAStatus vpu_vaCreateBuffer(VADriverContextP ctx, VAContextID context_id,
			    VABufferType type, unsigned int size,
			    unsigned int num_elements, void *data,
			    VABufferID *buf_id);
VAStatus vpu_vaBufferSetNumElements(VADriverContextP ctx, VABufferID buf_id,
				    unsigned int num_elements);
VAStatus vpu_vaMapBuffer(VADriverContextP ctx, VABufferID buf_id, void **pbuf);
VAStatus vpu_vaUnmapBuffer(VADriverContextP ctx, VABufferID buf_id);
VAStatus vpu_vaDestroyBuffer(VADriverContextP ctx, VABufferID buf_id);
VAStatus vpu_vaBufferInfo(VADriverContextP ctx, VABufferID buf_id,
			  VABufferType *type, unsigned int *size,
			  unsigned int *num_elements);
VAStatus vpu_vaBeginPicture(VADriverContextP ctx, VAContextID context_id,
			    VASurfaceID render_target);
VAStatus vpu_vaRenderPicture(VADriverContextP ctx, VAContextID context_id,
			     VABufferID *buffers, int num_buffers);
VAStatus vpu_vaEndPicture(VADriverContextP ctx, VAContextID context_id);
VAStatus vpu_vaSyncSurface(VADriverContextP ctx, VASurfaceID render_target);
VAStatus vpu_vaQuerySurfaceStatus(VADriverContextP ctx, VASurfaceID render_target,
				  VASurfaceStatus *status);
VAStatus vpu_vaQueryImageFormats(VADriverContextP ctx, VAImageFormat *format_list,
				 int *num_formats);
VAStatus vpu_vaCreateImage(VADriverContextP ctx, VAImageFormat *format, int width,
			   int height, VAImage *image);
VAStatus vpu_vaDeriveImage(VADriverContextP ctx, VASurfaceID surface,
			   VAImage *image);
VAStatus vpu_vaDestroyImage(VADriverContextP ctx, VAImageID image);
VAStatus vpu_vaSetImagePalette(VADriverContextP ctx, VAImageID image,
			       unsigned char *palette);
VAStatus vpu_vaGetImage(VADriverContextP ctx, VASurfaceID surface, int x, int y,
			unsigned int width, unsigned int height, VAImageID image);
VAStatus vpu_vaPutImage(VADriverContextP ctx, VASurfaceID surface, VAImageID image,
			int src_x, int src_y, unsigned int src_width,
			unsigned int src_height, int dest_x, int dest_y,
			unsigned int dest_width, unsigned int dest_height);

#endif
