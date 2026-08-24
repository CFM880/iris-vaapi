// SPDX-License-Identifier: GPL-2.0-or-later
/* Minimal stateful V4L2 M2M decoder engine for the Qualcomm Iris device.
 *
 * Feeds whole H.264/HEVC/VP9 access units to the OUTPUT (bitstream) queue and
 * returns decoded NV12 frames from the CAPTURE queue.  The CAPTURE queue is
 * set up only after the decoder reports the stream resolution through
 * V4L2_EVENT_SOURCE_CHANGE, mirroring the FFmpeg v4l2-m2m flow.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <stdint.h>

#include "v4l2_dec.h"

#define OUT_BUFFERS	16
#define CAP_BUFFERS	20

static unsigned int
out_sizeimage(unsigned int width, unsigned int height)
{
	uint64_t pixels = (uint64_t)width * height;
	uint64_t size = pixels * 2; /* generous compressed-frame working size */

	if (size < 1U * 1024 * 1024)
		size = 1U * 1024 * 1024;
	if (size > UINT32_MAX)
		size = UINT32_MAX;
	return (unsigned int)size;
}

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;

	do {
		r = ioctl(fd, req, arg);
	} while (r == -1 && errno == EINTR);

	return r;
}

static int v4l2_dec_mmap(struct v4l2_dec *d, enum v4l2_buf_type type)
{
	struct v4l2_buffer *meta;
	void ***mem;
	size_t **size;
	unsigned int count, i;

	if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
		meta = d->out_meta;
		mem = &d->out_mem;
		size = &d->out_size;
		count = d->out_count;
	} else {
		meta = d->cap_meta;
		mem = &d->cap_mem;
		size = &d->cap_size;
		count = d->cap_count;
	}

	*mem = calloc(count, sizeof(void *));
	*size = calloc(count, sizeof(size_t));
	if (!*mem || !*size)
		return -ENOMEM;

	for (i = 0; i < count; i++) {
		void *addr;

		addr = mmap(NULL, meta[i].m.planes[0].length,
			    PROT_READ | PROT_WRITE, MAP_SHARED, d->fd,
			    meta[i].m.planes[0].m.mem_offset);
		if (addr == MAP_FAILED)
			return -errno;
		(*mem)[i] = addr;
		(*size)[i] = meta[i].m.planes[0].length;
	}

	return 0;
}

/* Set up (or re-set-up) the CAPTURE queue using the driver's current format,
 * which is the default before the first DRC event and the real resolution
 * afterwards. */
static int v4l2_dec_setup_capture(struct v4l2_dec *d)
{
	struct v4l2_format fmt;
	struct v4l2_requestbuffers req;
	unsigned int i;

	/* Drop any previous CAPTURE buffers. */
	if (d->cap_meta) {
		if (d->streaming_cap) {
			enum v4l2_buf_type type =
				V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
			xioctl(d->fd, VIDIOC_STREAMOFF, &type);
			d->streaming_cap = 0;
		}
		for (i = 0; i < d->cap_count; i++) {
			if (d->cap_mem && d->cap_mem[i])
				munmap(d->cap_mem[i], d->cap_size[i]);
			free(d->cap_meta[i].m.planes);
		}
		free(d->cap_mem);
		free(d->cap_size);
		free(d->cap_meta);
		d->cap_mem = NULL;
		d->cap_size = NULL;
		d->cap_meta = NULL;
		d->cap_count = 0;

		memset(&req, 0, sizeof(req));
		req.count = 0;
		req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		req.memory = V4L2_MEMORY_MMAP;
		xioctl(d->fd, VIDIOC_REQBUFS, &req);
	}

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	fmt.fmt.pix_mp.width = d->width;
	fmt.fmt.pix_mp.height = d->height;
	fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
	fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
	fmt.fmt.pix_mp.num_planes = 1;
	if (xioctl(d->fd, VIDIOC_S_FMT, &fmt) < 0) {
		perror("S_FMT CAPTURE");
		return -errno;
	}
	d->cap_fmt = fmt;
	/* Keep the negotiated capture geometry, not merely the requested one.
	 * This matters after a V4L2 source-change event and for codecs whose
	 * coded dimensions are aligned differently at different resolutions. */
	d->width = fmt.fmt.pix_mp.width;
	d->height = fmt.fmt.pix_mp.height;

	memset(&req, 0, sizeof(req));
	req.count = CAP_BUFFERS;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	req.memory = V4L2_MEMORY_MMAP;
	if (xioctl(d->fd, VIDIOC_REQBUFS, &req) < 0) {
		perror("REQBUFS CAPTURE");
		return -errno;
	}
	d->cap_count = req.count;
	d->cap_meta = calloc(d->cap_count, sizeof(*d->cap_meta));
	if (!d->cap_meta)
		return -ENOMEM;
	for (i = 0; i < d->cap_count; i++) {
		memset(&d->cap_meta[i], 0, sizeof(d->cap_meta[i]));
		d->cap_meta[i].type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		d->cap_meta[i].memory = V4L2_MEMORY_MMAP;
		d->cap_meta[i].index = i;
		d->cap_meta[i].m.planes = calloc(1, sizeof(struct v4l2_plane));
		d->cap_meta[i].length = 1;
		if (xioctl(d->fd, VIDIOC_QUERYBUF, &d->cap_meta[i]) < 0) {
			perror("QUERYBUF CAPTURE");
			return -errno;
		}
	}
	if (v4l2_dec_mmap(d, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) < 0) {
		perror("mmap CAPTURE");
		return -ENOMEM;
	}

	for (i = 0; i < d->cap_count; i++) {
		memset(&d->cap_meta[i].m.planes[0], 0,
		       sizeof(d->cap_meta[i].m.planes[0]));
		d->cap_meta[i].m.planes[0].bytesused = 0;
		if (xioctl(d->fd, VIDIOC_QBUF, &d->cap_meta[i]) < 0) {
			perror("QBUF CAPTURE");
			return -errno;
		}
	}

	{
		enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

		if (xioctl(d->fd, VIDIOC_STREAMON, &type) < 0)
			return -errno;
		d->streaming_cap = 1;
	}

	printf("v4l2-dec: CAPTURE %ux%u sizeimage=%u bufs=%u\n", d->width,
	       d->height, fmt.fmt.pix_mp.plane_fmt[0].sizeimage, d->cap_count);
	return 0;
}

int v4l2_dec_open(struct v4l2_dec *d, const char *dev,
		  unsigned int width, unsigned int height,
		  unsigned int pixelformat)
{
	struct v4l2_capability cap;
	struct v4l2_requestbuffers req;
	struct v4l2_event_subscription sub;
	unsigned int i;

	memset(d, 0, sizeof(*d));
	d->fd = open(dev ? dev : "/dev/video0", O_RDWR | O_NONBLOCK);
	if (d->fd < 0) {
		perror("open /dev/video0");
		return -errno;
	}

	if (xioctl(d->fd, VIDIOC_QUERYCAP, &cap) < 0) {
		perror("QUERYCAP");
		return -errno;
	}
	if (!(cap.capabilities & V4L2_CAP_VIDEO_M2M_MPLANE)) {
		fprintf(stderr, "not an M2M mplane device: %s\n",
			(char *)cap.driver);
		return -EINVAL;
	}
	printf("v4l2-dec: device %s driver '%s' card '%s'\n", dev ? dev : "/dev/video0",
	       (char *)cap.driver, (char *)cap.card);

	memset(&sub, 0, sizeof(sub));
	sub.type = V4L2_EVENT_SOURCE_CHANGE;
	if (xioctl(d->fd, VIDIOC_SUBSCRIBE_EVENT, &sub) < 0)
		perror("SUBSCRIBE_EVENT SOURCE_CHANGE");

	d->width = width;
	d->height = height;

	/* ---- OUTPUT (bitstream) ---- */
	memset(&d->out_fmt, 0, sizeof(d->out_fmt));
	d->out_fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	d->out_fmt.fmt.pix_mp.width = width;
	d->out_fmt.fmt.pix_mp.height = height;
	d->out_fmt.fmt.pix_mp.pixelformat = pixelformat;
	d->out_fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
	d->out_fmt.fmt.pix_mp.num_planes = 1;
	d->out_fmt.fmt.pix_mp.plane_fmt[0].sizeimage =
		out_sizeimage(width, height);
	if (xioctl(d->fd, VIDIOC_S_FMT, &d->out_fmt) < 0) {
		perror("S_FMT OUTPUT");
		return -errno;
	}

	memset(&req, 0, sizeof(req));
	req.count = OUT_BUFFERS;
	req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	req.memory = V4L2_MEMORY_MMAP;
	if (xioctl(d->fd, VIDIOC_REQBUFS, &req) < 0) {
		perror("REQBUFS OUTPUT");
		return -errno;
	}
	d->out_count = req.count;
	d->out_meta = calloc(d->out_count, sizeof(*d->out_meta));
	if (!d->out_meta)
		return -ENOMEM;
	d->free_out = malloc(d->out_count * sizeof(*d->free_out));
	if (!d->free_out)
		return -ENOMEM;
	for (i = 0; i < d->out_count; i++)
		d->free_out[i] = d->out_count - 1 - i;
	d->free_out_n = d->out_count;
	for (i = 0; i < d->out_count; i++) {
		memset(&d->out_meta[i], 0, sizeof(d->out_meta[i]));
		d->out_meta[i].type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		d->out_meta[i].memory = V4L2_MEMORY_MMAP;
		d->out_meta[i].index = i;
		d->out_meta[i].m.planes = calloc(1, sizeof(struct v4l2_plane));
		d->out_meta[i].length = 1;
		if (xioctl(d->fd, VIDIOC_QUERYBUF, &d->out_meta[i]) < 0)
			return -errno;
	}
	if (v4l2_dec_mmap(d, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) < 0)
		return -ENOMEM;

	return 0;
}

int v4l2_dec_start(struct v4l2_dec *d)
{
	enum v4l2_buf_type type;

	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	if (xioctl(d->fd, VIDIOC_STREAMON, &type) < 0)
		return -errno;
	d->streaming = 1;

	return v4l2_dec_setup_capture(d);
}

/* poll the device for readable events / CAPTURE frames. Returns >0 ready. */
int v4l2_dec_poll(struct v4l2_dec *d, int timeout_ms)
{
	struct pollfd pfd;

	pfd.fd = d->fd;
	pfd.events = POLLIN | POLLOUT;
	pfd.revents = 0;
	return poll(&pfd, 1, timeout_ms);
}

int v4l2_dec_feed(struct v4l2_dec *d, const void *data, size_t len,
		  uint64_t timestamp)
{
	unsigned int idx;
	struct v4l2_buffer *b;

	if (d->free_out_n <= 0)
		return -EAGAIN;
	idx = d->free_out[--d->free_out_n];

	if (len == 0 || len > d->out_size[idx]) {
		fprintf(stderr, "v4l2-dec: access unit too large (%zu > %zu)\n",
			len, d->out_size[idx]);
		d->free_out[d->free_out_n++] = idx;
		return -E2BIG;
	}

	memcpy(d->out_mem[idx], data, len);
	b = &d->out_meta[idx];
	memset(&b->m.planes[0], 0, sizeof(b->m.planes[0]));
	b->m.planes[0].bytesused = len;
	b->timestamp.tv_sec = timestamp / 1000000000ULL;
	b->timestamp.tv_usec = (timestamp % 1000000000ULL) / 1000ULL;
	if (xioctl(d->fd, VIDIOC_QBUF, b) < 0) {
		d->free_out[d->free_out_n++] = idx;
		return -errno;
	}

	return 0;
}

/* Handle a DRC event if one is pending; returns 1 if CAPTURE was (re)built. */
int v4l2_dec_handle_events(struct v4l2_dec *d, int *changed)
{
	struct v4l2_event ev;
	int ret;

	*changed = 0;
	for (;;) {
		ret = xioctl(d->fd, VIDIOC_DQEVENT, &ev);
		if (ret < 0) {
			if (errno == EAGAIN || errno == ENOENT ||
			    errno == ENODEV)
				break;
			return -errno;
		}

		if (ev.type == V4L2_EVENT_SOURCE_CHANGE) {
			struct v4l2_format fmt;

			/* The first DRC report usually matches the CAPTURE format
			 * we already requested; only renegotiate on a real
			 * resolution change. */
			memset(&fmt, 0, sizeof(fmt));
			fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
			if (xioctl(d->fd, VIDIOC_G_FMT, &fmt) == 0 &&
			    fmt.fmt.pix_mp.width ==
					 d->cap_fmt.fmt.pix_mp.width &&
			    fmt.fmt.pix_mp.height ==
				    d->cap_fmt.fmt.pix_mp.height)
				continue;

			/* Re-read G_FMT inside and renegotiate CAPTURE. */
			d->width = fmt.fmt.pix_mp.width;
			d->height = fmt.fmt.pix_mp.height;
			ret = v4l2_dec_setup_capture(d);
			if (ret)
				return ret;
			*changed = 1;
		}
	}
	return 0;
}

/* Dequeue one CAPTURE frame; returns 0, 1 at EOS, -EAGAIN if none ready. */
int v4l2_dec_dqcap(struct v4l2_dec *d, struct v4l2_dec_frame *frame)
{
	struct v4l2_buffer b;

	memset(&b, 0, sizeof(b));
	b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	b.memory = V4L2_MEMORY_MMAP;
	b.length = 1;
	b.m.planes = calloc(1, sizeof(struct v4l2_plane));
	if (xioctl(d->fd, VIDIOC_DQBUF, &b) < 0) {
		if (errno == EAGAIN) {
			free(b.m.planes);
			return -EAGAIN;
		}
		perror("DQBUF CAPTURE");
		free(b.m.planes);
		return -errno;
	}

	frame->index = b.index;
	frame->bytesused = b.m.planes[0].bytesused;
	frame->width = d->width;
	frame->height = d->height;
	frame->mem = d->cap_mem[b.index];
	frame->flags = b.flags;
	frame->timestamp = b.timestamp.tv_sec * 1000000000ULL +
			   b.timestamp.tv_usec * 1000ULL;
	free(b.m.planes);

	if (b.flags & V4L2_BUF_FLAG_LAST)
		d->eos = 1;

	return d->eos ? 1 : 0;
}

int v4l2_dec_qcap_idx(struct v4l2_dec *d, unsigned int index)
{
	memset(&d->cap_meta[index].m.planes[0], 0,
	       sizeof(d->cap_meta[index].m.planes[0]));
	return xioctl(d->fd, VIDIOC_QBUF, &d->cap_meta[index]) < 0 ?
		-errno : 0;
}

int v4l2_dec_qcap(struct v4l2_dec *d, const struct v4l2_dec_frame *frame)
{
	return v4l2_dec_qcap_idx(d, frame->index);
}

int v4l2_dec_export(struct v4l2_dec *d, unsigned int cap_index, int *fd,
		    unsigned int *pitch, unsigned int *size)
{
	struct v4l2_exportbuffer exp;
	struct v4l2_plane *pl = &d->cap_meta[cap_index].m.planes[0];

	memset(&exp, 0, sizeof(exp));
	exp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	exp.index = cap_index;
	if (xioctl(d->fd, VIDIOC_EXPBUF, &exp) < 0)
		return -errno;
	*fd = exp.fd;
	*pitch = d->cap_fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
	*size = d->cap_size[cap_index];
	(void)pl;
	return 0;
}

void v4l2_dec_size(struct v4l2_dec *d, unsigned int *w, unsigned int *h)
{
	*w = d->width;
	*h = d->height;
}

/* Recycle a finished OUTPUT buffer so it can be reused. Returns 0 on success,
 * -EAGAIN if no OUTPUT buffer is ready yet. */
int v4l2_dec_dqout(struct v4l2_dec *d)
{
	struct v4l2_buffer b;

	memset(&b, 0, sizeof(b));
	b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	b.memory = V4L2_MEMORY_MMAP;
	b.length = 1;
	b.m.planes = calloc(1, sizeof(struct v4l2_plane));
	if (xioctl(d->fd, VIDIOC_DQBUF, &b) < 0) {
		if (errno == EAGAIN) {
			free(b.m.planes);
			return -EAGAIN;
		}
		free(b.m.planes);
		return -errno;
	}
	free(b.m.planes);
	d->free_out[d->free_out_n++] = b.index;
	return 0;
}

int v4l2_dec_flush(struct v4l2_dec *d)
{
	struct v4l2_buffer b;
	unsigned int idx;

	if (d->free_out_n <= 0)
		return -EAGAIN;
	idx = d->free_out[--d->free_out_n];

	memset(&b, 0, sizeof(b));
	b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	b.memory = V4L2_MEMORY_MMAP;
	b.length = 1;
	b.index = idx;
	b.m.planes = calloc(1, sizeof(struct v4l2_plane));
	b.m.planes[0].bytesused = 0;
	if (xioctl(d->fd, VIDIOC_QBUF, &b) < 0) {
		free(b.m.planes);
		d->free_out[d->free_out_n++] = idx;
		return -errno;
	}
	free(b.m.planes);
	return 0;
}

int v4l2_dec_stop(struct v4l2_dec *d)
{
	enum v4l2_buf_type type;

	if (d->streaming_cap) {
		type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		xioctl(d->fd, VIDIOC_STREAMOFF, &type);
		d->streaming_cap = 0;
	}
	if (d->streaming) {
		type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		xioctl(d->fd, VIDIOC_STREAMOFF, &type);
		d->streaming = 0;
	}
	return 0;
}

void v4l2_dec_close(struct v4l2_dec *d)
{
	unsigned int i;

	if (!d)
		return;
	v4l2_dec_stop(d);
	if (d->cap_meta) {
		for (i = 0; i < d->cap_count; i++) {
			if (d->cap_mem && d->cap_mem[i])
				munmap(d->cap_mem[i], d->cap_size[i]);
			free(d->cap_meta[i].m.planes);
		}
		free(d->cap_mem);
		free(d->cap_size);
		free(d->cap_meta);
	}
	if (d->out_meta) {
		for (i = 0; i < d->out_count; i++) {
			if (d->out_mem && d->out_mem[i])
				munmap(d->out_mem[i], d->out_size[i]);
			free(d->out_meta[i].m.planes);
		}
		free(d->out_mem);
		free(d->out_size);
		free(d->out_meta);
		free(d->free_out);
	}
	if (d->fd >= 0)
		close(d->fd);
}
