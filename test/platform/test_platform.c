// SPDX-License-Identifier: GPL-2.0-or-later

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "platform/platform.h"

int main(void)
{
	struct vpu_platform *platform;
	struct vpu_platform_session *session;

	assert(vpu_platform_create("does-not-exist", NULL) == NULL);
	platform = vpu_platform_create("qcom-iris", "/dev/null");
	assert(platform);
	assert(!strcmp(vpu_platform_name(platform), "qcom-iris"));
	assert(!strcmp(vpu_platform_device(platform), "/dev/null"));
	assert(vpu_platform_description(platform));
	assert(!vpu_platform_supports(platform, VPU_CODEC_H264,
				     VPU_PIXEL_FORMAT_NV12));

	session = vpu_platform_session_create(platform);
	assert(session);
	vpu_platform_session_destroy(session);
	vpu_platform_destroy(platform);
	puts("platform contract: ok");
	return 0;
}
