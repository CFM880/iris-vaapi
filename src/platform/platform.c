// SPDX-License-Identifier: GPL-2.0-or-later

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform_internal.h"

struct vpu_platform {
	const struct vpu_platform_ops *ops;
	char *device;
	unsigned char support[3][2];
};

struct vpu_platform_session {
	const struct vpu_platform *platform;
	void *private;
};

/* Adding a platform only requires implementing vpu_platform_ops and
 * registering it here.  Neither the VA frontend nor codec assembly changes. */
static const struct vpu_platform_ops *const platforms[] = {
	&qcom_vpu_platform_ops,
};

static const struct vpu_platform_ops *find_platform(const char *name)
{
	size_t i;

	for (i = 0; i < sizeof(platforms) / sizeof(platforms[0]); i++) {
		if (name && !strcmp(name, platforms[i]->name))
			return platforms[i];
	}
	return NULL;
}

static struct vpu_platform *create_platform(const struct vpu_platform_ops *ops,
					  const char *device)
{
	struct vpu_platform *platform;

	platform = calloc(1, sizeof(*platform));
	if (!platform)
		return NULL;
	platform->device = strdup(device);
	if (!platform->device) {
		free(platform);
		return NULL;
	}
	platform->ops = ops;
	for (int codec = VPU_CODEC_H264; codec <= VPU_CODEC_VP9; codec++) {
		platform->support[codec][0] = ops->supports(device, codec,
			VPU_PIXEL_FORMAT_NV12) != 0;
		platform->support[codec][1] = ops->supports(device, codec,
			VPU_PIXEL_FORMAT_P010) != 0;
	}
	return platform;
}

static int platform_has_capability(const struct vpu_platform *platform)
{
	int codec;

	for (codec = VPU_CODEC_H264; codec <= VPU_CODEC_VP9; codec++)
		if (platform->support[codec][0] || platform->support[codec][1])
			return 1;
	return 0;
}

struct vpu_platform *vpu_platform_create(const char *name, const char *device)
{
	const struct vpu_platform_ops *ops;
	struct vpu_platform *fallback = NULL;
	size_t i;

	if (!name || !*name)
		name = getenv("VPU_PLATFORM");
	if (!device || !*device)
		device = getenv("VPU_DEVICE");

	if (name && *name && strcmp(name, "auto")) {
		ops = find_platform(name);
		if (!ops) {
			fprintf(stderr, "vpu-vaapi: unknown platform '%s'\n",
				name);
			return NULL;
		}
		return create_platform(ops, device && *device ? device :
				      ops->default_device);
	}

	/* Auto-selection probes in registration order.  Retain the first platform
	 * as a diagnostic fallback when no device is currently available. */
	for (i = 0; i < sizeof(platforms) / sizeof(platforms[0]); i++) {
		struct vpu_platform *candidate = create_platform(platforms[i],
			device && *device ? device : platforms[i]->default_device);

		if (!candidate)
			continue;
		if (platform_has_capability(candidate)) {
			vpu_platform_destroy(fallback);
			return candidate;
		}
		if (!fallback)
			fallback = candidate;
		else
			vpu_platform_destroy(candidate);
	}
	return fallback;
}

void vpu_platform_destroy(struct vpu_platform *platform)
{
	if (!platform)
		return;
	free(platform->device);
	free(platform);
}

const char *vpu_platform_name(const struct vpu_platform *platform)
{
	return platform ? platform->ops->name : NULL;
}

const char *vpu_platform_description(const struct vpu_platform *platform)
{
	return platform ? platform->ops->description : NULL;
}

const char *vpu_platform_device(const struct vpu_platform *platform)
{
	return platform ? platform->device : NULL;
}

uint32_t vpu_platform_quirks(const struct vpu_platform *platform)
{
	return platform ? platform->ops->quirks : 0;
}

int vpu_platform_supports(const struct vpu_platform *platform,
			 enum vpu_codec_id codec, enum vpu_pixel_format format)
{
	unsigned int format_index;

	if (!platform || codec < VPU_CODEC_H264 || codec > VPU_CODEC_VP9)
		return 0;
	if (format == VPU_PIXEL_FORMAT_NV12)
		format_index = 0;
	else if (format == VPU_PIXEL_FORMAT_P010)
		format_index = 1;
	else
		return 0;
	return platform->support[codec][format_index];
}

struct vpu_platform_session *vpu_platform_session_create(const struct vpu_platform *platform)
{
	struct vpu_platform_session *session;

	if (!platform || !platform->ops->session_create)
		return NULL;
	session = calloc(1, sizeof(*session));
	if (!session)
		return NULL;
	session->private = platform->ops->session_create();
	if (!session->private) {
		free(session);
		return NULL;
	}
	session->platform = platform;
	return session;
}

void vpu_platform_session_destroy(struct vpu_platform_session *session)
{
	if (!session)
		return;
	session->platform->ops->session_destroy(session->private);
	free(session);
}

#define SESSION_CALL(session, method, ...) \
	((session) && (session)->platform->ops->method ? \
	 (session)->platform->ops->method((session)->private, __VA_ARGS__) : \
	 -ENOTSUP)

int vpu_platform_session_open(struct vpu_platform_session *session, unsigned int width,
		     unsigned int height, enum vpu_codec_id codec,
		     enum vpu_pixel_format format)
{
	if (!session || !session->platform->ops->open)
		return -ENOTSUP;
	return session->platform->ops->open(session->private,
		session->platform->device, width, height, codec, format);
}

void vpu_platform_session_close(struct vpu_platform_session *session)
{
	if (session && session->platform->ops->close)
		session->platform->ops->close(session->private);
}

int vpu_platform_session_set_capture_dmabufs(struct vpu_platform_session *session,
				    const int *fds, const size_t *sizes,
				    unsigned int count)
{
	return SESSION_CALL(session, set_capture_dmabufs, fds, sizes, count);
}

int vpu_platform_session_start(struct vpu_platform_session *session)
{
	if (!session || !session->platform->ops->start)
		return -ENOTSUP;
	return session->platform->ops->start(session->private);
}

int vpu_platform_session_poll(struct vpu_platform_session *session, int timeout_ms)
{
	return SESSION_CALL(session, poll, timeout_ms);
}

int vpu_platform_session_poll_capture(struct vpu_platform_session *session, int timeout_ms)
{
	return SESSION_CALL(session, poll_capture, timeout_ms);
}

int vpu_platform_session_submit(struct vpu_platform_session *session, const void *data,
		       size_t len, uint64_t timestamp)
{
	return SESSION_CALL(session, submit, data, len, timestamp);
}

int vpu_platform_session_handle_events(struct vpu_platform_session *session, int *changed)
{
	return SESSION_CALL(session, handle_events, changed);
}

int vpu_platform_session_dequeue_frame(struct vpu_platform_session *session,
			      struct vpu_decoded_frame *frame)
{
	return SESSION_CALL(session, dequeue_frame, frame);
}

int vpu_platform_session_requeue_frame(struct vpu_platform_session *session,
			      const struct vpu_decoded_frame *frame)
{
	return SESSION_CALL(session, requeue_frame, frame);
}

int vpu_platform_session_requeue_index(struct vpu_platform_session *session, unsigned int index)
{
	return SESSION_CALL(session, requeue_index, index);
}

int vpu_platform_session_export_frame(struct vpu_platform_session *session, unsigned int index,
			     int *fd, unsigned int *pitch,
			     unsigned int *size)
{
	return SESSION_CALL(session, export_frame, index, fd, pitch, size);
}

void vpu_platform_session_capture_layout(const struct vpu_platform_session *session,
				unsigned int *pitch, unsigned int *width,
				unsigned int *height)
{
	if (!session || !session->platform->ops->capture_layout)
		return;
	session->platform->ops->capture_layout(session->private, pitch, width,
					      height);
}

int vpu_platform_session_dequeue_input(struct vpu_platform_session *session)
{
	if (!session || !session->platform->ops->dequeue_input)
		return -ENOTSUP;
	return session->platform->ops->dequeue_input(session->private);
}

int vpu_platform_session_flush(struct vpu_platform_session *session)
{
	if (!session || !session->platform->ops->flush)
		return -ENOTSUP;
	return session->platform->ops->flush(session->private);
}

int vpu_platform_session_eos(const struct vpu_platform_session *session)
{
	if (!session || !session->platform->ops->eos)
		return 0;
	return session->platform->ops->eos(session->private);
}

int vpu_platform_session_capture_queued(const struct vpu_platform_session *session,
			       unsigned int index)
{
	if (!session || !session->platform->ops->capture_queued)
		return 0;
	return session->platform->ops->capture_queued(session->private, index);
}

int vpu_platform_session_attach_surface_fence(struct vpu_platform_session *session,
				     int dmabuf_fd, uint64_t token)
{
	return SESSION_CALL(session, attach_surface_fence, dmabuf_fd, token);
}

int vpu_platform_session_signal_surface_fence(struct vpu_platform_session *session,
				     uint64_t token)
{
	return SESSION_CALL(session, signal_surface_fence, token);
}
