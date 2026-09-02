// SPDX-License-Identifier: GPL-2.0-or-later

#include <errno.h>
#include <stdlib.h>

#include <linux/videodev2.h>

#include "../platform_internal.h"
#include "v4l2_decoder.h"

static unsigned int v4l2_codec_format(enum vpu_codec_id codec)
{
	switch (codec) {
	case VPU_CODEC_HEVC:
		return V4L2_PIX_FMT_HEVC;
	case VPU_CODEC_VP9:
		return V4L2_PIX_FMT_VP9;
	case VPU_CODEC_H264:
	default:
		return V4L2_PIX_FMT_H264;
	}
}

static int qcom_iris_supports(const char *device, enum vpu_codec_id codec,
				 enum vpu_pixel_format format)
{
	return v4l2_dec_supports_output_format(device,
					       v4l2_codec_format(codec)) &&
	       v4l2_dec_supports_capture_format(device, (unsigned int)format);
}

static void *qcom_iris_session_create(void)
{
	struct v4l2_dec *dec = calloc(1, sizeof(*dec));

	if (dec)
		dec->fd = -1;
	return dec;
}

static void qcom_iris_session_destroy(void *session)
{
	free(session);
}

static int qcom_iris_open(void *session, const char *device,
			     unsigned int width, unsigned int height,
			     enum vpu_codec_id codec,
			     enum vpu_pixel_format format)
{
	return v4l2_dec_open(session, device, width, height,
			     v4l2_codec_format(codec), (unsigned int)format);
}

static int qcom_iris_dequeue_frame(void *session, struct vpu_decoded_frame *frame)
{
	struct v4l2_dec_frame native;
	int ret = v4l2_dec_dqcap(session, &native);

	if (ret < 0)
		return ret;
	frame->index = native.index;
	frame->bytesused = native.bytesused;
	frame->width = native.width;
	frame->height = native.height;
	frame->mem = native.mem;
	frame->flags = native.flags;
	frame->timestamp = native.timestamp;
	frame->capture_generation = native.capture_generation;
	return ret;
}

static void qcom_iris_close(void *session)
{
	v4l2_dec_close(session);
}

static int qcom_iris_set_capture_dmabufs(void *session, const int *fds,
					    const size_t *sizes,
					    unsigned int count)
{
	return v4l2_dec_set_capture_dmabufs(session, fds, sizes, count);
}

static int qcom_iris_start(void *session)
{
	return v4l2_dec_start(session);
}

static int qcom_iris_poll(void *session, int timeout_ms)
{
	return v4l2_dec_poll(session, timeout_ms);
}

static int qcom_iris_poll_capture(void *session, int timeout_ms)
{
	return v4l2_dec_poll_cap(session, timeout_ms);
}

static int qcom_iris_submit(void *session, const void *data, size_t len,
			       uint64_t timestamp)
{
	return v4l2_dec_feed(session, data, len, timestamp);
}

static int qcom_iris_handle_events(void *session, int *changed)
{
	return v4l2_dec_handle_events(session, changed);
}

static int qcom_iris_requeue_frame(void *session,
				      const struct vpu_decoded_frame *frame)
{
	return v4l2_dec_qcap_idx(session, frame->index);
}

static int qcom_iris_requeue_index(void *session, unsigned int index)
{
	return v4l2_dec_qcap_idx(session, index);
}

static int qcom_iris_export_frame(void *session, unsigned int index,
				     int *fd, unsigned int *pitch,
				     unsigned int *size)
{
	return v4l2_dec_export(session, index, fd, pitch, size);
}

static int qcom_iris_dequeue_input(void *session)
{
	return v4l2_dec_dqout(session);
}

static int qcom_iris_flush(void *session)
{
	return v4l2_dec_flush(session);
}

static int qcom_iris_eos(const void *session)
{
	return ((const struct v4l2_dec *)session)->eos;
}

static int qcom_iris_capture_queued(const void *session,
				       unsigned int index)
{
	const struct v4l2_dec *dec = session;

	return dec->cap_queued && index < dec->cap_count &&
	       dec->cap_queued[index];
}

static int qcom_iris_attach_surface_fence(void *session, int dmabuf_fd,
					     uint64_t token)
{
	return v4l2_dec_attach_surface_fence(session, dmabuf_fd, token);
}

static int qcom_iris_signal_surface_fence(void *session, uint64_t token)
{
	return v4l2_dec_signal_surface_fence(session, token);
}

static void qcom_iris_capture_layout(const void *session,
					unsigned int *pitch,
					unsigned int *width,
					unsigned int *height)
{
	const struct v4l2_dec *dec = session;

	if (pitch)
		*pitch = dec->cap_fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
	if (width)
		*width = dec->cap_fmt.fmt.pix_mp.width;
	if (height)
		*height = dec->cap_fmt.fmt.pix_mp.height;
}

const struct vpu_platform_ops qcom_vpu_platform_ops = {
	.name = "qcom-iris",
	.description = "Qualcomm Iris stateful V4L2 M2M",
	.default_device = "/dev/video0",
	.quirks = VPU_PLATFORM_QUIRK_HEVC_CAPTURE_FIFO |
		  VPU_PLATFORM_QUIRK_VP9_RELEASE_AU,
	.supports = qcom_iris_supports,
	.session_create = qcom_iris_session_create,
	.session_destroy = qcom_iris_session_destroy,
	.open = qcom_iris_open,
	.close = qcom_iris_close,
	.set_capture_dmabufs = qcom_iris_set_capture_dmabufs,
	.start = qcom_iris_start,
	.poll = qcom_iris_poll,
	.poll_capture = qcom_iris_poll_capture,
	.submit = qcom_iris_submit,
	.handle_events = qcom_iris_handle_events,
	.dequeue_frame = qcom_iris_dequeue_frame,
	.requeue_frame = qcom_iris_requeue_frame,
	.requeue_index = qcom_iris_requeue_index,
	.export_frame = qcom_iris_export_frame,
	.capture_layout = qcom_iris_capture_layout,
	.dequeue_input = qcom_iris_dequeue_input,
	.flush = qcom_iris_flush,
	.eos = qcom_iris_eos,
	.capture_queued = qcom_iris_capture_queued,
	.attach_surface_fence = qcom_iris_attach_surface_fence,
	.signal_surface_fence = qcom_iris_signal_surface_fence,
};
