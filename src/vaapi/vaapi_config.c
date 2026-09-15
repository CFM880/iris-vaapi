// SPDX-License-Identifier: GPL-2.0-or-later
/* VA config objects and the profile/capability queries they are built on. */

#include "vaapi_internal.h"

int
vpu_profile_is_10bit(VAProfile profile)
{
	enum vpu_pixel_format format;

	return !vpu_codec_profile_info(profile, NULL, &format, NULL) &&
	       format == VPU_PIXEL_FORMAT_P010;
}

unsigned int
vpu_profile_fourcc(VAProfile profile)
{
	return vpu_profile_is_10bit(profile) ? VA_FOURCC_P010 : VA_FOURCC_NV12;
}

unsigned int
vpu_profile_rt_format(VAProfile profile)
{
	return vpu_profile_is_10bit(profile) ?
		VA_RT_FORMAT_YUV420_10 : VA_RT_FORMAT_YUV420;
}

int
vpu_profile_known(VAProfile profile)
{
	return vpu_codec_profile_info(profile, NULL, NULL, NULL) == 0;
}

enum vpu_codec_id
vpu_profile_codec(VAProfile profile)
{
	enum vpu_codec_id codec = VPU_CODEC_H264;

	(void)vpu_codec_profile_info(profile, &codec, NULL, NULL);
	return codec;
}

int
vpu_profile_supported(struct vpu_drv_data *dd, VAProfile profile)
{
	enum vpu_pixel_format format;

	if (!vpu_profile_known(profile) || !dd || !dd->platform)
		return 0;
	format = vpu_profile_is_10bit(profile) ? VPU_PIXEL_FORMAT_P010 :
		VPU_PIXEL_FORMAT_NV12;
	return vpu_platform_supports(dd->platform, vpu_profile_codec(profile),
				    format);
}

VAStatus
vpu_vaQueryConfigProfiles(VADriverContextP ctx, VAProfile *profile_list,
			   int *num_profiles)
{
	struct vpu_drv_data *dd;
	int count = 0;
	unsigned int i;

	if (!num_profiles)
		return VA_STATUS_ERROR_INVALID_PARAMETER;
	dd = vpu_drv_data(ctx);
	if (!dd)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;

	for (i = 0; i < vpu_codec_profile_count(); i++) {
		VAProfile profile = vpu_codec_profile_at(i);

		if (!vpu_profile_supported(dd, profile))
			continue;
		if (profile_list)
			profile_list[count] = profile;
		count++;
	}
	*num_profiles = count;
	return VA_STATUS_SUCCESS;
}

VAStatus
vpu_vaQueryConfigEntrypoints(VADriverContextP ctx, VAProfile profile,
			      VAEntrypoint *entrypoint_list, int *num_entrypoints)
{
	struct vpu_drv_data *dd;

	if (!num_entrypoints)
		return VA_STATUS_ERROR_INVALID_PARAMETER;
	dd = vpu_drv_data(ctx);
	if (!dd)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	if (!vpu_profile_supported(dd, profile)) {
		*num_entrypoints = 0;
		return VA_STATUS_SUCCESS;
	}

	*num_entrypoints = VPU_NUM_ENTRYPOINTS;
	if (entrypoint_list) {
		if (*num_entrypoints > (int)VPU_NUM_ENTRYPOINTS)
			*num_entrypoints = VPU_NUM_ENTRYPOINTS;
		memcpy(entrypoint_list, vpu_entrypoints,
		       *num_entrypoints * sizeof(*entrypoint_list));
	}
	return VA_STATUS_SUCCESS;
}

VAStatus
vpu_vaGetConfigAttributes(VADriverContextP ctx, VAProfile profile,
			   VAEntrypoint entrypoint, VAConfigAttrib *attrib_list,
			   int num_attribs)
{
	struct vpu_drv_data *dd;
	int i;

	if (entrypoint != VAEntrypointVLD)
		return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
	dd = vpu_drv_data(ctx);
	if (!dd)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	if (!vpu_profile_supported(dd, profile))
		return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
	if (num_attribs < 0 || (num_attribs && !attrib_list))
		return VA_STATUS_ERROR_INVALID_PARAMETER;
	for (i = 0; i < num_attribs; i++) {
		switch (attrib_list[i].type) {
		case VAConfigAttribRTFormat:
			attrib_list[i].value = vpu_profile_rt_format(profile);
			break;
		case VAConfigAttribDecProcessing:
			attrib_list[i].value = 0;
			break;
		case VAConfigAttribMaxPictureWidth:
		case VAConfigAttribMaxPictureHeight:
			attrib_list[i].value = 4096;
			break;
		default:
			attrib_list[i].value = VA_ATTRIB_NOT_SUPPORTED;
			break;
		}
	}
	return VA_STATUS_SUCCESS;
}

VAStatus
vpu_vaCreateConfig(VADriverContextP ctx, VAProfile profile,
		    VAEntrypoint entrypoint, VAConfigAttrib *attrib_list,
		    int num_attribs, VAConfigID *config_id)
{
	struct vpu_drv_data *dd;
	int i;

	if (entrypoint != VAEntrypointVLD)
		return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
	if (!config_id)
		return VA_STATUS_ERROR_INVALID_PARAMETER;
	if (num_attribs < 0 || (num_attribs && !attrib_list))
		return VA_STATUS_ERROR_INVALID_PARAMETER;

	dd = vpu_drv_data(ctx);
	if (!dd)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	if (!vpu_profile_supported(dd, profile))
		return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
	if (dd->n_configs >= VPU_MAX_CONFIGS)
		return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
	for (i = 0; i < num_attribs; i++) {
		if (attrib_list[i].type == VAConfigAttribRTFormat &&
		    !(attrib_list[i].value & vpu_profile_rt_format(profile)))
			return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
		if (attrib_list[i].type == VAConfigAttribDecProcessing &&
		    attrib_list[i].value)
			return VA_STATUS_ERROR_UNSUPPORTED_FILTER;
	}

	*config_id = ++dd->config_id;
	dd->configs[dd->n_configs].id = *config_id;
	dd->configs[dd->n_configs].profile = profile;
	dd->n_configs++;
	fprintf(stderr, "[cfg] created config=%u profile=%d\n", *config_id,
		profile);
	return VA_STATUS_SUCCESS;
}

VAStatus
vpu_vaDestroyConfig(VADriverContextP ctx, VAConfigID config_id)
{
	struct vpu_drv_data *dd = ctx ? ctx->pDriverData : NULL;
	int i;

	if (!dd)
		return VA_STATUS_ERROR_INVALID_CONFIG;
	for (i = 0; i < dd->n_configs; i++) {
		if (dd->configs[i].id != config_id)
			continue;
		dd->configs[i] = dd->configs[dd->n_configs - 1];
		dd->n_configs--;
		return VA_STATUS_SUCCESS;
	}
	return VA_STATUS_ERROR_INVALID_CONFIG;
}

VAStatus
vpu_vaQueryConfigAttributes(VADriverContextP ctx, VAConfigID config_id,
			     VAProfile *profile, VAEntrypoint *entrypoint,
			     VAConfigAttrib *attrib_list, int *num_attribs)
{
	struct vpu_drv_data *dd = ctx ? ctx->pDriverData : NULL;
	struct vpu_config *cfg;

	if (!dd || !profile || !entrypoint || !num_attribs)
		return VA_STATUS_ERROR_INVALID_PARAMETER;
	cfg = vpu_find_config(dd, config_id);
	if (!cfg)
		return VA_STATUS_ERROR_INVALID_CONFIG;
	*profile = cfg->profile;
	*entrypoint = VAEntrypointVLD;
	if (attrib_list) {
		/* num_attribs is an output here (clients pass uninitialized
		 * garbage); always report one supported RT format. */
		attrib_list[0].type = VAConfigAttribRTFormat;
		attrib_list[0].value = vpu_profile_rt_format(cfg->profile);
		*num_attribs = 1;
	} else {
		*num_attribs = 1;
	}
	return VA_STATUS_SUCCESS;
}
