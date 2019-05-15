/*
 * Copyright 2015 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: AMD
 *
 */

#include <drm/drm_crtc.h>
#include <drm/drm_vblank.h>

#include "amdgpu.h"
#include "amdgpu_dm.h"
#include "dc.h"

static enum amdgpu_dm_pipe_crc_source dm_parse_crc_source(const char *source)
{
	if (!source || !strcmp(source, "none"))
		return AMDGPU_DM_PIPE_CRC_SOURCE_NONE;
	if (!strcmp(source, "auto") || !strcmp(source, "crtc"))
		return AMDGPU_DM_PIPE_CRC_SOURCE_CRTC;
	if (!strcmp(source, "dprx"))
		return AMDGPU_DM_PIPE_CRC_SOURCE_DPRX;

	return AMDGPU_DM_PIPE_CRC_SOURCE_INVALID;
}

int
amdgpu_dm_crtc_verify_crc_source(struct drm_crtc *crtc, const char *src_name,
				 size_t *values_cnt)
{
	enum amdgpu_dm_pipe_crc_source source = dm_parse_crc_source(src_name);

	if (source < 0) {
		DRM_DEBUG_DRIVER("Unknown CRC source %s for CRTC%d\n",
				 src_name, crtc->index);
		return -EINVAL;
	}

	*values_cnt = 3;
	return 0;
}

int amdgpu_dm_crtc_set_crc_source(struct drm_crtc *crtc, const char *src_name)
{
	struct amdgpu_device *adev = crtc->dev->dev_private;
	struct dm_crtc_state *crtc_state = to_dm_crtc_state(crtc->state);
	struct dc_stream_state *stream_state = crtc_state->stream;
	struct amdgpu_dm_connector *aconn;
	struct drm_dp_aux *aux = NULL;
	bool enable = false;
	bool enabled = false;

	enum amdgpu_dm_pipe_crc_source source = dm_parse_crc_source(src_name);

	if (source < 0) {
		DRM_DEBUG_DRIVER("Unknown CRC source %s for CRTC%d\n",
				 src_name, crtc->index);
		return -EINVAL;
	}

	if (!stream_state) {
		DRM_ERROR("No stream state for CRTC%d\n", crtc->index);
		return -EINVAL;
	}

	enable = amdgpu_dm_is_valid_crc_source(source);

	mutex_lock(&adev->dm.dc_lock);
	/*
	 * USER REQ SRC | CURRENT SRC | BEHAVIOR
	 * -----------------------------
	 * None         | None        | Do nothing
	 * None         | CRTC        | Disable CRTC CRC
	 * None         | DPRX        | Disable DPRX CRC, need 'aux'
	 * CRTC         | XXXX        | Enable CRTC CRC, configure DC strm
	 * DPRX         | XXXX        | Enable DPRX CRC, need 'aux'
	 */
	if (source == AMDGPU_DM_PIPE_CRC_SOURCE_DPRX ||
		(source == AMDGPU_DM_PIPE_CRC_SOURCE_NONE &&
		 crtc_state->crc_src == AMDGPU_DM_PIPE_CRC_SOURCE_DPRX)) {
		aconn = stream_state->link->priv;

		if (!aconn) {
			DRM_DEBUG_DRIVER("No amd connector matching CRTC-%d\n", crtc->index);
			mutex_unlock(&adev->dm.dc_lock);
			return -EINVAL;
		}

		aux = &aconn->dm_dp_aux.aux;

		if (!aux) {
			DRM_DEBUG_DRIVER("No dp aux for amd connector\n");
			mutex_unlock(&adev->dm.dc_lock);
			return -EINVAL;
		}
	} else if (source == AMDGPU_DM_PIPE_CRC_SOURCE_CRTC) {
		if (!dc_stream_configure_crc(stream_state->ctx->dc, stream_state,
					     enable, enable)) {
			mutex_unlock(&adev->dm.dc_lock);
			return -EINVAL;
		}
	}

	/* When enabling CRC, we should also disable dithering. */
	dc_stream_set_dither_option(stream_state,
				    enable ? DITHER_OPTION_TRUN8
					   : DITHER_OPTION_DEFAULT);

	mutex_unlock(&adev->dm.dc_lock);

	/*
	 * Reading the CRC requires the vblank interrupt handler to be
	 * enabled. Keep a reference until CRC capture stops.
	 */
	enabled = amdgpu_dm_is_valid_crc_source(crtc_state->crc_src);
	if (!enabled && enable) {
		drm_crtc_vblank_get(crtc);
		if (source == AMDGPU_DM_PIPE_CRC_SOURCE_DPRX) {
			if (drm_dp_start_crc(aux, crtc)) {
				DRM_DEBUG_DRIVER("dp start crc failed\n");
				return -EINVAL;
			}
		}
	} else if (enabled && !enable) {
		drm_crtc_vblank_put(crtc);
		if (crtc_state->crc_src == AMDGPU_DM_PIPE_CRC_SOURCE_DPRX) {
			if (drm_dp_stop_crc(aux)) {
				DRM_DEBUG_DRIVER("dp stop crc failed\n");
				return -EINVAL;
			}
		}
	}

	crtc_state->crc_src = source;

	/* Reset crc_skipped on dm state */
	crtc_state->crc_skip_count = 0;
	return 0;
}

/**
 * amdgpu_dm_crtc_handle_crc_irq: Report to DRM the CRC on given CRTC.
 * @crtc: DRM CRTC object.
 *
 * This function should be called at the end of a vblank, when the fb has been
 * fully processed through the pipe.
 */
void amdgpu_dm_crtc_handle_crc_irq(struct drm_crtc *crtc)
{
	struct dm_crtc_state *crtc_state;
	struct dc_stream_state *stream_state;
	uint32_t crcs[3];

	if (crtc == NULL)
		return;

	crtc_state = to_dm_crtc_state(crtc->state);
	stream_state = crtc_state->stream;

	/* Early return if CRC capture is not enabled. */
	if (!amdgpu_dm_is_valid_crc_source(crtc_state->crc_src))
		return;

	/*
	 * Since flipping and crc enablement happen asynchronously, we - more
	 * often than not - will be returning an 'uncooked' crc on first frame.
	 * Probably because hw isn't ready yet. For added security, skip the
	 * first two CRC values.
	 */
	if (crtc_state->crc_skip_count < 2) {
		crtc_state->crc_skip_count += 1;
		return;
	}

	if (crtc_state->crc_src == AMDGPU_DM_PIPE_CRC_SOURCE_CRTC) {
		if (!dc_stream_get_crc(stream_state->ctx->dc, stream_state,
				       &crcs[0], &crcs[1], &crcs[2]))
			return;

		drm_crtc_add_crc_entry(crtc, true,
				       drm_crtc_accurate_vblank_count(crtc), crcs);
	}
}
