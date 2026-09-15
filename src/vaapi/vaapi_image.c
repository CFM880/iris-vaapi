// SPDX-License-Identifier: GPL-2.0-or-later
/* VA image objects: explicit CPU transfer images (vaCreateImage/vaGetImage)
 * and lightweight derived views over a surface backing (vaDeriveImage). */

#include "vaapi_internal.h"

static unsigned int
vpu_surface_pitch(unsigned int width, unsigned int fourcc)
{
	unsigned int bytes = fourcc == VA_FOURCC_P010 ? 2 : 1;
	unsigned int alignment = fourcc == VA_FOURCC_P010 ? 256 : 128;

	return ALIGN(width * bytes, alignment);
}

VAStatus
vpu_vaQueryImageFormats(VADriverContextP ctx, VAImageFormat *format_list,
			 int *num_formats)
{
	struct vpu_drv_data *dd;
	static const VAImageFormat nv12 = {
		.fourcc = VA_FOURCC_NV12,
		.byte_order = VA_LSB_FIRST,
		.bits_per_pixel = 12,
	};
	static const VAImageFormat p010 = {
		.fourcc = VA_FOURCC_P010,
		.byte_order = VA_LSB_FIRST,
		.bits_per_pixel = 24,
	};

	if (!num_formats)
		return VA_STATUS_ERROR_INVALID_PARAMETER;
	dd = vpu_drv_data(ctx);
	if (!dd)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	if (format_list) {
		format_list[0] = nv12;
		if (dd->p010_supported)
			format_list[1] = p010;
	}
	*num_formats = dd->p010_supported ? 2 : 1;
	return VA_STATUS_SUCCESS;
}

VAStatus
vpu_vaCreateImage(VADriverContextP ctx, VAImageFormat *format, int width,
		   int height, VAImage *image)
{
	struct vpu_drv_data *dd = ctx->pDriverData;
	unsigned int pitch, size;
	VABufferID bid;

	if (!dd || !format || !image)
		return VA_STATUS_ERROR_INVALID_PARAMETER;
	if (dd->img_n >= 64)
		return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
	if (format->fourcc != VA_FOURCC_NV12 &&
	    format->fourcc != VA_FOURCC_P010)
		return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
	if (format->fourcc == VA_FOURCC_P010 && !dd->p010_supported)
		return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;

	/* Match the stable surface/CAPTURE layout used by the selected platform. */
	pitch = vpu_surface_pitch(width, format->fourcc);
	size = (unsigned int)pitch * ALIGN(height, 32) * 3 / 2;
	dd->img_data[dd->img_n] = calloc(1, size);
	if (!dd->img_data[dd->img_n])
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	bid = ++dd->buffer_id;
	dd->img_ids[dd->img_n] = bid;
	dd->img_sizes[dd->img_n] = size;
	dd->img_ws[dd->img_n] = width;
	dd->img_hs[dd->img_n] = height;
	dd->img_fourcc[dd->img_n] = format->fourcc;

	memset(image, 0, sizeof(*image));
	image->image_id = bid;
	image->width = width;
	image->height = height;
	image->format = *format;
	image->data_size = size;
	image->num_planes = 2;
	image->pitches[0] = pitch;
	image->pitches[1] = pitch;
	image->offsets[0] = 0;
	image->offsets[1] = (unsigned int)pitch * ALIGN(height, 32);
	image->buf = bid;
	dd->img_n++;
	return VA_STATUS_SUCCESS;
}

VAStatus
vpu_vaDeriveImage(VADriverContextP ctx, VASurfaceID surface, VAImage *image)
{
	struct vpu_drv_data *dd = ctx->pDriverData;
	unsigned int pitch, size, w, h, fourcc;
	void *mem;
	VABufferID bid;

	if (!dd || !image)
		return VA_STATUS_ERROR_INVALID_SURFACE;
	if (vpu_surfaces_buffer(dd->surfs, surface, &mem, &pitch, &size,
			      &w, &h, &fourcc))
		return VA_STATUS_ERROR_INVALID_SURFACE;
	if (dd->derived_n >= 256)
		return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;

	bid = ++dd->buffer_id;
	dd->derived_ids[dd->derived_n] = bid;
	dd->derived_mem[dd->derived_n] = mem;
	dd->derived_n++;

	memset(image, 0, sizeof(*image));
	image->image_id = bid;
	image->width = w;
	image->height = h;
	image->format.fourcc = fourcc;
	image->format.byte_order = VA_LSB_FIRST;
	image->format.bits_per_pixel = fourcc == VA_FOURCC_P010 ? 24 : 12;
	image->data_size = size;
	image->num_planes = 2;
	image->pitches[0] = pitch;
	image->pitches[1] = pitch;
	image->offsets[0] = 0;
	image->offsets[1] = pitch * h;
	image->buf = bid;
	return VA_STATUS_SUCCESS;
}

VAStatus
vpu_vaDestroyImage(VADriverContextP ctx, VAImageID image)
{
	struct vpu_drv_data *dd = ctx->pDriverData;
	int i;

	if (!dd)
		return VA_STATUS_ERROR_INVALID_IMAGE;
	for (i = 0; i < dd->derived_n; i++) {
		if (dd->derived_ids[i] != image)
			continue;
		dd->derived_ids[i] = dd->derived_ids[dd->derived_n - 1];
		dd->derived_mem[i] = dd->derived_mem[dd->derived_n - 1];
		dd->derived_n--;
		return VA_STATUS_SUCCESS;
	}
	for (i = 0; i < dd->img_n; i++) {
		if (dd->img_ids[i] != image)
			continue;
		free(dd->img_data[i]);
		dd->img_ids[i] = dd->img_ids[dd->img_n - 1];
		dd->img_data[i] = dd->img_data[dd->img_n - 1];
		dd->img_sizes[i] = dd->img_sizes[dd->img_n - 1];
		dd->img_ws[i] = dd->img_ws[dd->img_n - 1];
		dd->img_hs[i] = dd->img_hs[dd->img_n - 1];
		dd->img_fourcc[i] = dd->img_fourcc[dd->img_n - 1];
		dd->img_n--;
		return VA_STATUS_SUCCESS;
	}
	return VA_STATUS_ERROR_INVALID_IMAGE;
}

VAStatus
vpu_vaSetImagePalette(VADriverContextP ctx, VAImageID image,
		       unsigned char *palette)
{
	return VA_STATUS_SUCCESS;
}

VAStatus
vpu_vaGetImage(VADriverContextP ctx, VASurfaceID surface, int x, int y,
		unsigned int width, unsigned int height, VAImageID image)
{
	struct vpu_drv_data *dd = ctx->pDriverData;
	unsigned int pitch, size, cap_w, cap_h, fourcc;
	int img_i = -1;
	void *mem, *dst = NULL;
	int i;

	if (!dd)
		return VA_STATUS_ERROR_INVALID_PARAMETER;
	if (!dd->surfs || !vpu_surfaces_valid(dd->surfs, surface))
		return VA_STATUS_ERROR_INVALID_SURFACE;
	i = vpu_surfaces_sync(dd->surfs, surface);
	if (i)
		return i == -ETIMEDOUT ? VA_STATUS_ERROR_TIMEDOUT :
			VA_STATUS_ERROR_DECODING_ERROR;
	if (vpu_surfaces_buffer(dd->surfs, surface, &mem, &pitch, &size,
			      &cap_w, &cap_h, &fourcc))
		return VA_STATUS_ERROR_INVALID_SURFACE;
	for (i = 0; i < dd->img_n; i++)
		if (dd->img_ids[i] == image) {
			dst = dd->img_data[i];
			img_i = i;
			break;
		}
	if (!dst)
		return VA_STATUS_ERROR_INVALID_IMAGE;
	if (dd->img_fourcc[img_i] != fourcc)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	/* The image was created with the aligned coded layout; the buffer may
	 * carry the CAPTURE-negotiated one.  Copy row-wise between them so
	 * diverging strides cannot garble rows or overrun the image. */
	{
		unsigned int img_w = dd->img_ws[img_i];
		unsigned int img_h = dd->img_hs[img_i];
		unsigned int img_pitch = vpu_surface_pitch(img_w, fourcc);
		unsigned int img_chroma = img_pitch * ALIGN(img_h, 32);
		unsigned int bytes = fourcc == VA_FOURCC_P010 ? 2 : 1;
		unsigned int row_bytes;
		const uint8_t *src;
		uint8_t *d = dst;

		if (x < 0 || y < 0 || (x | y | width | height) & 1 ||
		    width > img_w || height > img_h ||
		    (unsigned int)x > cap_w || (unsigned int)y > cap_h ||
		    width > cap_w - (unsigned int)x ||
		    height > cap_h - (unsigned int)y)
			return VA_STATUS_ERROR_INVALID_PARAMETER;
		row_bytes = width * bytes;
		src = (const uint8_t *)mem + (unsigned int)y * pitch +
		      (unsigned int)x * bytes;
		for (i = 0; i < (int)height; i++)
			memcpy(d + (unsigned int)i * img_pitch,
			       src + (unsigned int)i * pitch, row_bytes);
		src = (const uint8_t *)mem + pitch * cap_h +
		      (unsigned int)(y / 2) * pitch + (unsigned int)x * bytes;
		d = dst + img_chroma;
		for (i = 0; i < (int)(height / 2); i++)
			memcpy(d + (unsigned int)i * img_pitch,
			       src + (unsigned int)i * pitch, row_bytes);
	}
	return VA_STATUS_SUCCESS;
}

VAStatus
vpu_vaPutImage(VADriverContextP ctx, VASurfaceID surface, VAImageID image,
		int src_x, int src_y, unsigned int src_width,
		unsigned int src_height, int dest_x, int dest_y,
		unsigned int dest_width, unsigned int dest_height)
{
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}
