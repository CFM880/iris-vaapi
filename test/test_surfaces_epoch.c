// SPDX-License-Identifier: GPL-2.0-or-later
/* Stream-epoch surface invalidation: a surface decoded before a seek must
 * never be presented with its pre-seek pixels.  After a boundary the driver
 * backfills stale surfaces with the newest frame of the current epoch (repeat
 * last frame) or neutral black when no such frame exists yet. */
#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../src/decode/decode_internal.h"

int main(void)
{
	struct vpu_surfaces t = {0};
	struct vpu_decode_ctx ctx = { .surfs = &t };
	const unsigned int w = 64, h = 64;
	const unsigned int fourcc = VPU_PIXEL_FORMAT_NV12;
	unsigned int pitch = surface_pitch(w, fourcc);
	size_t size = (size_t)pitch * ALIGN_TO(h, 32) * 3 / 2;
	struct vpu_surface *s1 = &t.s[0], *s2 = &t.s[1];
	uint8_t *a = malloc(size), *b = malloc(size);

	assert(a && b);
	t.n = 2;
	s1->id = 10;
	s2->id = 11;
	for (int i = 0; i < 2; i++) {
		struct vpu_surface *s = &t.s[i];

		s->sw = w;
		s->sh = h;
		s->fourcc = fourcc;
		s->bsize = size;
		s->bmap = i ? b : a;
		s->bfd = open("/dev/null", O_RDWR);
		s->epoch = t.epoch;
		assert(s->bfd >= 0);
	}
	memset(a, 0x11, size);	/* picture decoded before the seek */
	s1->decoded = 1;
	s1->queued = 1;
	s1->owner = &ctx;

	/* Seek: abandon the stream.  No current-epoch source yet, so the stale
	 * surface must become neutral black instead of the old picture. */
	surfs_begin_epoch(&t);
	assert(vpu_surfaces_sync(&t, s1->id) == 0);
	assert(a[0] == 16 && a[size - 1] == 128);

	/* First frames of the new epoch: s2 is the newest. */
	memset(a, 0x33, size);
	surfaces_mark_decoded(&ctx, s1);
	memset(b, 0x22, size);
	surfaces_mark_decoded(&ctx, s2);

	/* Another boundary; s1 is stale and must repeat s2, not its own pixels. */
	surfs_begin_epoch(&t);
	memset(b, 0x55, size);
	surfaces_mark_decoded(&ctx, s2);	/* newest frame of this epoch */
	assert(vpu_surfaces_sync(&t, s1->id) == 0);
	assert(a[0] == 0x55 && a[size - 1] == 0x55);

	/* A surface decoded in the current epoch is presented unchanged. */
	memset(a, 0x77, size);
	surfaces_mark_decoded(&ctx, s1);
	assert(vpu_surfaces_sync(&t, s1->id) == 0);
	assert(a[0] == 0x77);

	puts("surface stream-epoch invalidation: ok");
	free(a);
	free(b);
	return 0;
}
