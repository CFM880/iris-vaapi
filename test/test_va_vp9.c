// SPDX-License-Identifier: GPL-2.0-or-later
/* End-to-end VP9 decode test through the iris-vaapi driver.
 * Feeds frames of an IVF file (each frame is one VASliceDataBufferType),
 * then syncs a frame and exports it.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>

static int feed_frame(VADisplay dpy, VAContextID ctx, VASurfaceID surf,
		      const void *data, size_t len)
{
	VABufferID bufs[2];
	VABufferID rb[1];
	VAStatus st;

	st = vaCreateBuffer(dpy, ctx, VASliceDataBufferType, len, 1,
			    (void *)data, &bufs[0]);
	if (st) { fprintf(stderr, "slice buf: %s\n", vaErrorStr(st)); return -1; }
	rb[0] = bufs[0];
	st = vaBeginPicture(dpy, ctx, surf);
	if (st) { fprintf(stderr, "begin: %s\n", vaErrorStr(st)); return -1; }
	st = vaRenderPicture(dpy, ctx, rb, 1);
	if (st) { fprintf(stderr, "render: %s\n", vaErrorStr(st)); return -1; }
	st = vaEndPicture(dpy, ctx);
	if (st) { fprintf(stderr, "end: %s\n", vaErrorStr(st)); return -1; }
	vaDestroyBuffer(dpy, bufs[0]);
	return 0;
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/tmp/test-vp9.ivf";
	unsigned int width = argc > 2 ? atoi(argv[2]) : 1920;
	unsigned int height = argc > 3 ? atoi(argv[3]) : 1088;
	unsigned char *stream;
	long fsize;
	FILE *fp;
	VADisplay dpy;
	VAConfigID cfg;
	VAContextID ctx;
	VASurfaceID surf[24];
	VAStatus st;
	int major = 0, minor = 0, fd;
	unsigned int n_frames = 0;
	unsigned char *p, *end;

	fp = fopen(path, "rb");
	if (!fp) { perror(path); return 1; }
	fseek(fp, 0, SEEK_END);
	fsize = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	stream = malloc(fsize);
	if (!stream || fread(stream, 1, fsize, fp) != (size_t)fsize)
		return 1;
	fclose(fp);

	fd = open("/dev/dri/renderD128", O_RDWR);
	if (fd < 0) { perror("open renderD128"); return 1; }
	dpy = vaGetDisplayDRM(fd);
	if (!dpy) { fprintf(stderr, "vaGetDisplayDRM\n"); return 1; }
	st = vaInitialize(dpy, &major, &minor);
	if (st != VA_STATUS_SUCCESS) {
		fprintf(stderr, "vaInitialize: %s\n", vaErrorStr(st));
		return 1;
	}

	st = vaCreateConfig(dpy, VAProfileVP9Profile0, VAEntrypointVLD, NULL, 0,
			    &cfg);
	if (st) { fprintf(stderr, "vaCreateConfig: %s\n", vaErrorStr(st)); return 1; }
	st = vaCreateContext(dpy, cfg, width, height, 0, NULL, 0, &ctx);
	if (st) { fprintf(stderr, "vaCreateContext: %s\n", vaErrorStr(st)); return 1; }
	st = vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, width, height, surf, 24,
			      NULL, 0);
	if (st) { fprintf(stderr, "vaCreateSurfaces: %s\n", vaErrorStr(st)); return 1; }

	/* Parse IVF: 32-byte header, then per-frame 4-byte size + 8-byte ts. */
	p = stream + 32;
	end = stream + fsize;
	while (p + 12 <= end) {
		unsigned int sz = p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);

		if (sz > 16 * 1024 * 1024 || p + 12 + sz > end) {
			fprintf(stderr, "bad frame size %u at %u\n", sz, n_frames);
			break;
		}
		if (feed_frame(dpy, ctx, surf[n_frames % 24], p + 12, sz)) {
			fprintf(stderr, "feed failed at frame %u\n", n_frames);
			break;
		}
		n_frames++;
		p += 12 + sz;
	}
	free(stream);
	printf("fed %u frames\n", n_frames);

	/* Sync the most recently fed surface (the firmware holds the last
	 * picture until the next feed or EOS). */
	{
		int sync_idx = n_frames - 1;
		VASurfaceID s = surf[sync_idx % 24];

		st = vaSyncSurface(dpy, s);
		if (st) { fprintf(stderr, "vaSyncSurface %d: %s\n", sync_idx, vaErrorStr(st)); return 1; }
		printf("synced surface %u\n", s);

		{
			VADRMPRIMESurfaceDescriptor dsc;
			char outpath[64];
			void *mp;
			FILE *out;

			st = vaExportSurfaceHandle(dpy, s,
						   VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
						   VA_EXPORT_SURFACE_READ_ONLY, &dsc);
			if (st) { fprintf(stderr, "export: %s\n", vaErrorStr(st)); return 1; }
			mp = mmap(NULL, dsc.objects[0].size, PROT_READ, MAP_SHARED,
				  dsc.objects[0].fd, 0);
			if (mp == MAP_FAILED) { perror("mmap"); return 1; }
			snprintf(outpath, sizeof(outpath), "/tmp/va-vp9-%d.nv12", sync_idx);
			out = fopen(outpath, "wb");
			if (out) { fwrite(mp, 1, dsc.objects[0].size, out); fclose(out); }
			else perror("fopen");
			printf("wrote %s (%u bytes)\n", outpath, dsc.objects[0].size);
			munmap(mp, dsc.objects[0].size);
		}
	}

	vaDestroyConfig(dpy, cfg);
	vaTerminate(dpy);
	close(fd);
	printf("VP9 DECODE OK\n");
	return 0;
}