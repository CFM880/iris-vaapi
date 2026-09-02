// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef VPU_VAAPI_PLATFORM_INTERNAL_H
#define VPU_VAAPI_PLATFORM_INTERNAL_H

#include "platform.h"

struct vpu_platform_ops {
	const char *name;
	const char *description;
	const char *default_device;
	uint32_t quirks;

	int (*supports)(const char *device, enum vpu_codec_id codec,
			enum vpu_pixel_format format);
	void *(*session_create)(void);
	void (*session_destroy)(void *session);
	int (*open)(void *session, const char *device, unsigned int width,
		    unsigned int height, enum vpu_codec_id codec,
		    enum vpu_pixel_format format);
	void (*close)(void *session);
	int (*set_capture_dmabufs)(void *session, const int *fds,
				   const size_t *sizes, unsigned int count);
	int (*start)(void *session);
	int (*poll)(void *session, int timeout_ms);
	int (*poll_capture)(void *session, int timeout_ms);
	int (*submit)(void *session, const void *data, size_t len,
		      uint64_t timestamp);
	int (*handle_events)(void *session, int *changed);
	int (*dequeue_frame)(void *session, struct vpu_decoded_frame *frame);
	int (*requeue_frame)(void *session, const struct vpu_decoded_frame *frame);
	int (*requeue_index)(void *session, unsigned int index);
	int (*export_frame)(void *session, unsigned int index, int *fd,
			    unsigned int *pitch, unsigned int *size);
	void (*capture_layout)(const void *session, unsigned int *pitch,
			       unsigned int *width, unsigned int *height);
	int (*dequeue_input)(void *session);
	int (*flush)(void *session);
	int (*eos)(const void *session);
	int (*capture_queued)(const void *session, unsigned int index);
	int (*attach_surface_fence)(void *session, int dmabuf_fd,
				    uint64_t token);
	int (*signal_surface_fence)(void *session, uint64_t token);
};

extern const struct vpu_platform_ops qcom_vpu_platform_ops;

#endif
