// SPDX-License-Identifier: GPL-2.0-or-later
/* VA buffer objects: the per-display table of CPU-side parameter/slice data.
 *
 * vaMapBuffer also resolves image and derived-image ids, which live in the
 * same struct vpu_drv_data and are managed by vaapi_image.c. */

#include "vaapi_internal.h"

void
vpu_free_buffers(struct vpu_drv_data *dd)
{
	int i;

	for (i = 0; i < dd->n_bufs; i++)
		free(dd->buf_data[i]);
	dd->n_bufs = 0;
}

int
vpu_find_buffer(struct vpu_drv_data *dd, VABufferID buf_id)
{
	int i;

	for (i = 0; i < dd->n_bufs; i++)
		if (dd->buf_ids[i] == buf_id)
			return i;
	return -1;
}

VAStatus
vpu_vaCreateBuffer(VADriverContextP ctx, VAContextID context_id,
		    VABufferType type, unsigned int size, unsigned int num_elements,
		    void *data, VABufferID *buf_id)
{
	struct vpu_drv_data *dd;

	dd = vpu_drv_data(ctx);
	if (!dd)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	if (dd->n_bufs >= 256)
		return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;

	dd->buf_types[dd->n_bufs] = type;
	dd->buf_num_elements[dd->n_bufs] = num_elements ? num_elements : 1;
	if (size > UINT_MAX / dd->buf_num_elements[dd->n_bufs])
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	dd->buf_sizes[dd->n_bufs] = size * dd->buf_num_elements[dd->n_bufs];
	dd->buf_data[dd->n_bufs] = calloc(1, dd->buf_sizes[dd->n_bufs] ?
						 dd->buf_sizes[dd->n_bufs] : 1);
	if (!dd->buf_data[dd->n_bufs])
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	if (data && dd->buf_sizes[dd->n_bufs])
		memcpy(dd->buf_data[dd->n_bufs], data, dd->buf_sizes[dd->n_bufs]);
	dd->buf_ids[dd->n_bufs] = ++dd->buffer_id;
	*buf_id = dd->buf_ids[dd->n_bufs];
	dd->n_bufs++;
	return VA_STATUS_SUCCESS;
}

VAStatus
vpu_vaBufferSetNumElements(VADriverContextP ctx, VABufferID buf_id,
			    unsigned int num_elements)
{
	return VA_STATUS_SUCCESS;
}

VAStatus
vpu_vaMapBuffer(VADriverContextP ctx, VABufferID buf_id, void **pbuf)
{
	struct vpu_drv_data *dd = ctx->pDriverData;
	int i;

	*pbuf = NULL;
	if (!dd)
		return VA_STATUS_ERROR_INVALID_BUFFER;

	for (i = 0; i < dd->derived_n; i++) {
		if (dd->derived_ids[i] != buf_id)
			continue;
		*pbuf = dd->derived_mem[i];
		return VA_STATUS_SUCCESS;
	}

	for (i = 0; i < dd->img_n; i++) {
		if (dd->img_ids[i] != buf_id)
			continue;
		*pbuf = dd->img_data[i];
		return VA_STATUS_SUCCESS;
	}

	i = vpu_find_buffer(dd, buf_id);
	if (i < 0)
		return VA_STATUS_ERROR_INVALID_BUFFER;
	*pbuf = dd->buf_data[i];
	return VA_STATUS_SUCCESS;
}

VAStatus
vpu_vaUnmapBuffer(VADriverContextP ctx, VABufferID buf_id)
{
	return VA_STATUS_SUCCESS;
}

VAStatus
vpu_vaDestroyBuffer(VADriverContextP ctx, VABufferID buf_id)
{
	struct vpu_drv_data *dd = ctx->pDriverData;
	int i;

	if (!dd)
		return VA_STATUS_SUCCESS;
	i = vpu_find_buffer(dd, buf_id);
	if (i < 0)
		return VA_STATUS_ERROR_INVALID_BUFFER;
	free(dd->buf_data[i]);
	dd->buf_ids[i] = dd->buf_ids[dd->n_bufs - 1];
	dd->buf_types[i] = dd->buf_types[dd->n_bufs - 1];
	dd->buf_sizes[i] = dd->buf_sizes[dd->n_bufs - 1];
	dd->buf_num_elements[i] = dd->buf_num_elements[dd->n_bufs - 1];
	dd->buf_data[i] = dd->buf_data[dd->n_bufs - 1];
	dd->n_bufs--;
	return VA_STATUS_SUCCESS;
}

VAStatus
vpu_vaBufferInfo(VADriverContextP ctx, VABufferID buf_id,
		  VABufferType *type, unsigned int *size, unsigned int *num_elements)
{
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}
