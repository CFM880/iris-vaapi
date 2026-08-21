// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Minimal libva client to verify a VA-API driver registers and advertises
 * decode profiles/entrypoints (stand-in for vainfo, P0 check).
 */

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#include <va/va.h>
#include <va/va_drm.h>

static const char *profile_name(VAProfile p)
{
	switch (p) {
	case VAProfileH264ConstrainedBaseline: return "H264ConstrainedBaseline";
	case VAProfileH264Main: return "H264Main";
	case VAProfileH264High: return "H264High";
	case VAProfileHEVCMain: return "HEVCMain";
	case VAProfileVP9Profile0: return "VP9Profile0";
	default: return "?";
	}
}

static const char *entrypoint_name(VAEntrypoint e)
{
	switch (e) {
	case VAEntrypointVLD: return "VLD";
	case VAEntrypointEncSlice: return "EncSlice";
	default: return "?";
	}
}

int main(int argc, char **argv)
{
	const char *drm_dev = argc > 1 ? argv[1] : "/dev/dri/renderD128";
	VADisplay dpy;
	VAStatus st;
	int major, minor, i;
	VAProfile profiles[32];
	int n_profiles = 32;
	VAEntrypoint ep[8];
	int n_ep = 8;

	int fd = open(drm_dev, O_RDWR);
	if (fd < 0) {
		perror("open drm");
		return 1;
	}

	dpy = vaGetDisplayDRM(fd);
	if (!dpy) {
		fprintf(stderr, "vaGetDisplayDRM failed\n");
		return 1;
	}

	st = vaInitialize(dpy, &major, &minor);
	if (st != VA_STATUS_SUCCESS) {
		fprintf(stderr, "vaInitialize failed: %s\n", vaErrorStr(st));
		return 1;
	}
	printf("VA-API version %d.%d\n", major, minor);
	printf("Vendor: %s\n", vaQueryVendorString(dpy));

	st = vaQueryConfigProfiles(dpy, profiles, &n_profiles);
	if (st != VA_STATUS_SUCCESS) {
		fprintf(stderr, "vaQueryConfigProfiles failed: %s\n", vaErrorStr(st));
		return 1;
	}
	printf("%d profiles:\n", n_profiles);
	for (i = 0; i < n_profiles; i++) {
		n_ep = 8;
		st = vaQueryConfigEntrypoints(dpy, profiles[i], ep, &n_ep);
		if (st != VA_STATUS_SUCCESS) {
			fprintf(stderr, "entrypoints failed for %s\n",
				profile_name(profiles[i]));
			continue;
		}
		printf("  %-24s entrypoints:", profile_name(profiles[i]));
		for (int j = 0; j < n_ep; j++)
			printf(" %s", entrypoint_name(ep[j]));
		printf("\n");
	}

	vaTerminate(dpy);
	close(fd);
	return 0;
}