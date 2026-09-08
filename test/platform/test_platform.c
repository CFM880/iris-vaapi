// SPDX-License-Identifier: GPL-2.0-or-later

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
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

	/* An explicit API path wins over the environment; an environment path
	 * wins over discovery, even if it has no decoder capabilities. */
	assert(setenv("VPU_DEVICE", "/dev/zero", 1) == 0);
	platform = vpu_platform_create("qcom-iris", "/dev/null");
	assert(platform);
	assert(!strcmp(vpu_platform_device(platform), "/dev/null"));
	vpu_platform_destroy(platform);
	platform = vpu_platform_create("auto", NULL);
	assert(platform);
	assert(!strcmp(vpu_platform_device(platform), "/dev/zero"));
	assert(!vpu_platform_supports(platform, VPU_CODEC_H264,
				     VPU_PIXEL_FORMAT_NV12));
	vpu_platform_destroy(platform);
	unsetenv("VPU_DEVICE");
	puts("platform contract: ok");
	return 0;
}
