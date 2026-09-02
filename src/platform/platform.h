// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Platform contract for a stateful access-unit VPU decoder.
 *
 * The VA-API and codec layers submit complete access units through this API.
 * Platform implementations own device discovery, queueing, decoded-frame
 * buffers and platform-specific synchronization operations.
 */

#ifndef VPU_VAAPI_PLATFORM_H
#define VPU_VAAPI_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#include "codec/types.h"

struct vpu_platform;
struct vpu_platform_session;

struct vpu_decoded_frame {
	unsigned int index;
	unsigned int bytesused;
	unsigned int width;
	unsigned int height;
	void *mem;
	uint32_t flags;
	uint64_t timestamp;
	uint64_t capture_generation;
};

enum vpu_platform_quirk {
	/* CAPTURE timestamps cannot map decoded HEVC frames to submissions. */
	VPU_PLATFORM_QUIRK_HEVC_CAPTURE_FIFO = 1U << 0,
	/* VP9 needs a following internal AU before the current frame is emitted. */
	VPU_PLATFORM_QUIRK_VP9_RELEASE_AU = 1U << 1,
};

/* Select a platform. NULL arguments use VPU_PLATFORM/VPU_DEVICE,
 * then fall back to the first registered platform and its default device. */
struct vpu_platform *vpu_platform_create(const char *name,
				       const char *device);
void vpu_platform_destroy(struct vpu_platform *platform);
const char *vpu_platform_name(const struct vpu_platform *platform);
const char *vpu_platform_description(const struct vpu_platform *platform);
const char *vpu_platform_device(const struct vpu_platform *platform);
uint32_t vpu_platform_quirks(const struct vpu_platform *platform);
int vpu_platform_supports(const struct vpu_platform *platform,
			 enum vpu_codec_id codec, enum vpu_pixel_format format);

struct vpu_platform_session *vpu_platform_session_create(
	const struct vpu_platform *platform);
void vpu_platform_session_destroy(struct vpu_platform_session *session);
int vpu_platform_session_open(struct vpu_platform_session *session,
			       unsigned int width, unsigned int height,
			       enum vpu_codec_id codec,
			       enum vpu_pixel_format format);
void vpu_platform_session_close(struct vpu_platform_session *session);

int vpu_platform_session_set_capture_dmabufs(struct vpu_platform_session *session,
				    const int *fds, const size_t *sizes,
				    unsigned int count);
int vpu_platform_session_start(struct vpu_platform_session *session);
int vpu_platform_session_poll(struct vpu_platform_session *session, int timeout_ms);
int vpu_platform_session_poll_capture(struct vpu_platform_session *session,
				       int timeout_ms);
int vpu_platform_session_submit(struct vpu_platform_session *session,
				 const void *data, size_t len,
				 uint64_t timestamp);
int vpu_platform_session_handle_events(struct vpu_platform_session *session,
					int *changed);
int vpu_platform_session_dequeue_frame(struct vpu_platform_session *session,
			      struct vpu_decoded_frame *frame);
int vpu_platform_session_requeue_frame(struct vpu_platform_session *session,
			      const struct vpu_decoded_frame *frame);
int vpu_platform_session_requeue_index(struct vpu_platform_session *session,
			      unsigned int index);
int vpu_platform_session_export_frame(struct vpu_platform_session *session,
			     unsigned int index, int *fd,
			     unsigned int *pitch, unsigned int *size);
void vpu_platform_session_capture_layout(const struct vpu_platform_session *session,
				unsigned int *pitch, unsigned int *width,
				unsigned int *height);
int vpu_platform_session_dequeue_input(struct vpu_platform_session *session);
int vpu_platform_session_flush(struct vpu_platform_session *session);
int vpu_platform_session_eos(const struct vpu_platform_session *session);
int vpu_platform_session_capture_queued(const struct vpu_platform_session *session,
			       unsigned int index);

/* Optional explicit-fence support. Platforms return -ENOTSUP when their
 * platform has no equivalent; the surface layer then uses CPU DMA-BUF sync. */
int vpu_platform_session_attach_surface_fence(struct vpu_platform_session *session,
				     int dmabuf_fd, uint64_t token);
int vpu_platform_session_signal_surface_fence(struct vpu_platform_session *session,
				     uint64_t token);

#endif
