// SPDX-License-Identifier: GPL-2.0-or-later
/* Minimal stateful V4L2 M2M decoder engine for the Qualcomm Iris device.
 *
 * Feeds whole H.264/HEVC/VP9 access units to the OUTPUT (bitstream) queue and
 * returns decoded NV12 frames from the CAPTURE queue, including resolution
 * renegotiation on V4L2_EVENT_SOURCE_CHANGE.
 */

#ifndef IRIS_VAAPI_V4L2_DEC_H
#define IRIS_VAAPI_V4L2_DEC_H

#include <linux/videodev2.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>

struct v4l2_dec_frame {
	unsigned int index;		/* CAPTURE buffer index */
	unsigned int bytesused;
	unsigned int width, height;
	void *mem;			/* mmap base, single NV12 plane */
	__u32 flags;			/* V4L2_BUF_FLAG_* */
	__u64 timestamp;
};

struct v4l2_dec {
	int fd;
	unsigned int width, height;	/* stream dimensions */

	struct v4l2_format out_fmt;
	struct v4l2_format cap_fmt;

	struct v4l2_buffer *out_meta;
	struct v4l2_buffer *cap_meta;
	void **out_mem;
	void **cap_mem;
	size_t *out_size;
	size_t *cap_size;
	unsigned int out_count;
	unsigned int cap_count;
	unsigned int *free_out;		/* free OUTPUT buffer indices */
	int free_out_n;

	int streaming;			/* OUTPUT queue streamed */
	int streaming_cap;		/* CAPTURE queue streamed */
	int cap_changed;		/* renegotiated since last check */
	int eos;
};

/* Open /dev/video0 (or @dev) for H.264 decode of a @width x @height stream. */
int v4l2_dec_open(struct v4l2_dec *d, const char *dev,
		  unsigned int width, unsigned int height);
/* STREAMON the OUTPUT queue (CAPTURE comes up after the first DRC event). */
int v4l2_dec_start(struct v4l2_dec *d);
/* poll the device fd. Returns poll() result. */
int v4l2_dec_poll(struct v4l2_dec *d, int timeout_ms);
/* Queue one full access unit on the OUTPUT queue.  The decoder propagates
 * @timestamp to the produced CAPTURE frame, which the driver uses to match
 * decoded frames back to VA surfaces. */
int v4l2_dec_feed(struct v4l2_dec *d, const void *data, size_t len,
		  uint64_t timestamp);
/* Handle pending events; builds/rebuilds CAPTURE after a resolution change. */
int v4l2_dec_handle_events(struct v4l2_dec *d, int *changed);
/* Dequeue a decoded NV12 frame. Returns 0, 1 at EOS, -EAGAIN if none ready. */
int v4l2_dec_dqcap(struct v4l2_dec *d, struct v4l2_dec_frame *frame);
/* Requeue a consumed CAPTURE buffer. */
int v4l2_dec_qcap(struct v4l2_dec *d, const struct v4l2_dec_frame *frame);
/* Requeue a CAPTURE buffer by index. */
int v4l2_dec_qcap_idx(struct v4l2_dec *d, unsigned int index);
/* Export a CAPTURE buffer as a DRM PRIME fd and return its luma pitch. */
int v4l2_dec_export(struct v4l2_dec *d, unsigned int cap_index, int *fd,
		    unsigned int *pitch, unsigned int *size);
/* Current coded size of the CAPTURE queue. */
void v4l2_dec_size(struct v4l2_dec *d, unsigned int *w, unsigned int *h);
/* Dequeue + requeue a finished OUTPUT buffer. -EAGAIN if none ready. */
int v4l2_dec_dqout(struct v4l2_dec *d);
/* Signal EOS (queue an empty LAST buffer). */
int v4l2_dec_flush(struct v4l2_dec *d);
int v4l2_dec_stop(struct v4l2_dec *d);
void v4l2_dec_close(struct v4l2_dec *d);

#endif