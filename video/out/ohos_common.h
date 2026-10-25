/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <libplacebo/colorspace.h>
#include <native_buffer/buffer_common.h>
#include <native_window/external_window.h>

#include "common/common.h"

struct vo;
struct vo_frame;
struct mp_image_params;
struct pl_color_map_params;

// Values of --ohos-hdr-mode.
enum ohos_hdr_mode {
    // HDR output signalled from the stream. HDR Vivid, the only HarmonyOS
    // format with dynamic metadata, also carries Dolby Vision and HDR10+.
    OHOS_HDR_MODE_AUTO = 0,
    // SDR output: libplacebo tone maps HDR streams down.
    OHOS_HDR_MODE_OFF,
    // HDR output with static metadata only, for panels without HDR Vivid.
    OHOS_HDR_MODE_HDR10,
};

bool vo_ohos_init(struct vo *vo);
void vo_ohos_uninit(struct vo *vo);
OHNativeWindow *vo_ohos_native_window(struct vo *vo);
bool vo_ohos_ensure_surface_size(struct vo *vo, int *w, int *h);
struct pl_color_space vo_ohos_preferred_csp(struct vo *vo);
bool vo_ohos_set_color(struct vo *vo, struct mp_image_params *params);
void vo_ohos_invalidate_color(struct vo *vo);

// Captures the HDR description of the frame about to be drawn: its colour
// space and dynamic metadata, plus the colour mapping libplacebo renders it
// with. The next vo_ohos_set_color() derives the NativeWindow metadata from
// these, so that it describes the rendered output. A frame without an image
// keeps the previous frame's description.
void vo_ohos_set_frame(struct vo *vo, const struct vo_frame *frame,
                       const struct pl_color_map_params *color_map);

// A seek or a new file (VOCTRL_RESET): the dynamic metadata of the stream
// shown so far is dropped once a frame from the new position arrives, the
// peak it has shown at once.
void vo_ohos_reset(struct vo *vo);

// The peak libplacebo's detection (--hdr-compute-peak) holds after the frame
// just drawn, smoothed over the scene, as a PQ value. The brightest of the
// stream so far raises the window's light levels where the stream's metadata
// understates them.
void vo_ohos_set_measured_peak(struct vo *vo, float max_pq_y);
