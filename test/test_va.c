// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Minimal libva client to verify a VA-API driver registers and advertises
 * decode profiles/entrypoints (stand-in for vainfo).
 */

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>

static const char *profile_name(VAProfile p)
{
	switch (p) {
	case VAProfileH264ConstrainedBaseline: return "H264ConstrainedBaseline";
	case VAProfileH264Main: return "H264Main";
	case VAProfileH264High: return "H264High";
	case VAProfileHEVCMain: return "HEVCMain";
	case VAProfileHEVCMain10: return "HEVCMain10";
	case VAProfileVP9Profile0: return "VP9Profile0";
	case VAProfileVP9Profile2: return "VP9Profile2";
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

	/* Exercise the 10-bit allocation and export path without starting the
	 * decoder.  This catches P010 pitch/size/fourcc regressions on any host. */
	{
		VAConfigAttrib attr = { .type = VAConfigAttribRTFormat };
		VADRMPRIMESurfaceDescriptor desc;
		VAConfigID config;
		VASurfaceID surface;
		VAImage image;

		st = vaGetConfigAttributes(dpy, VAProfileVP9Profile2,
					   VAEntrypointVLD, &attr, 1);
		if (st != VA_STATUS_SUCCESS ||
		    !(attr.value & VA_RT_FORMAT_YUV420_10)) {
			fprintf(stderr, "VP9 Profile 2 lacks YUV420_10\n");
			return 1;
		}
		st = vaCreateConfig(dpy, VAProfileVP9Profile2, VAEntrypointVLD,
				    &attr, 1, &config);
		if (st != VA_STATUS_SUCCESS) {
			fprintf(stderr, "Profile 2 config failed: %s\n", vaErrorStr(st));
			return 1;
		}
		st = vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420_10, 384, 216,
				      &surface, 1, NULL, 0);
		if (st != VA_STATUS_SUCCESS) {
			fprintf(stderr, "P010 surface failed: %s\n", vaErrorStr(st));
			return 1;
		}
		st = vaDeriveImage(dpy, surface, &image);
		if (st != VA_STATUS_SUCCESS || image.format.fourcc != VA_FOURCC_P010 ||
		    image.pitches[0] != 768 || image.data_size != 258048) {
			fprintf(stderr, "invalid P010 derived image layout\n");
			return 1;
		}
		vaDestroyImage(dpy, image.image_id);

		memset(&desc, 0, sizeof(desc));
		st = vaExportSurfaceHandle(dpy, surface,
					   VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
					   VA_EXPORT_SURFACE_SEPARATE_LAYERS,
					   &desc);
		if (st != VA_STATUS_SUCCESS || desc.fourcc != VA_FOURCC_P010 ||
		    desc.num_objects != 1 || desc.num_layers != 2 ||
		    desc.layers[0].pitch[0] != 768 ||
		    desc.layers[1].offset[0] != 768 * 224) {
			fprintf(stderr, "invalid P010 DRM PRIME layout\n");
			return 1;
		}
		close(desc.objects[0].fd);
		vaDestroySurfaces(dpy, &surface, 1);
		vaDestroyConfig(dpy, config);
		printf("P010 surface/export: OK\n");
	}

	vaTerminate(dpy);
	close(fd);
	return 0;
}
