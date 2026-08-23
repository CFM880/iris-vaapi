// SPDX-License-Identifier: GPL-2.0-or-later
/* HEVC engine test: feed an Annex-B HEVC stream through the iris V4L2
 * decoder and count decoded NV12 frames.
 *
 * Frame boundaries in HEVC streams are not reliably detectable from the
 * slice first_slice_segment_in_pic_flag (some encoders interleave reference
 * and non-reference slices per picture), so the whole stream is fed as one
 * buffer and the stateful firmware does its own picture parsing.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "v4l2_dec.h"

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/tmp/test-hevc.h265";
	unsigned int width = argc > 2 ? atoi(argv[2]) : 1920;
	unsigned int height = argc > 3 ? atoi(argv[3]) : 1088;
	unsigned char *stream;
	long fsize;
	FILE *fp;
	struct v4l2_dec dec;
	struct v4l2_dec_frame frame;
	int ret, changed;
	unsigned int decoded = 0;
	struct timespec t0, t1;
	double secs;

	fp = fopen(path, "rb");
	if (!fp) { perror(path); return 1; }
	fseek(fp, 0, SEEK_END);
	fsize = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	stream = malloc(fsize);
	if (!stream || fread(stream, 1, fsize, fp) != (size_t)fsize)
		return 1;
	fclose(fp);

	ret = v4l2_dec_open(&dec, "/dev/video0", width, height,
			    V4L2_PIX_FMT_HEVC);
	if (ret) { fprintf(stderr, "open %d\n", ret); return 1; }

	ret = v4l2_dec_feed(&dec, stream, fsize, 1000);
	if (ret) { fprintf(stderr, "feed %d\n", ret); return 1; }
	ret = v4l2_dec_start(&dec);
	if (ret) { fprintf(stderr, "start %d\n", ret); return 1; }

	clock_gettime(CLOCK_MONOTONIC, &t0);

	for (;;) {
		ret = v4l2_dec_poll(&dec, 2000);
		if (ret <= 0)
			break;
		while (v4l2_dec_dqout(&dec) == 0)
			;
		v4l2_dec_handle_events(&dec, &changed);
		ret = v4l2_dec_dqcap(&dec, &frame);
		if (ret < 0)
			break;
		if (ret == 1)
			break;
		decoded++;
		v4l2_dec_qcap(&dec, &frame);
	}

	clock_gettime(CLOCK_MONOTONIC, &t1);
	secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
	printf("decoded %u frames, %.1f fps\n", decoded,
	       secs > 0 ? decoded / secs : 0);

	v4l2_dec_close(&dec);
	free(stream);
	return 0;
}