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

#include <libavutil/frame.h>

#include "ohos_common.h"
#include "common/msg.h"
#include "video/mp_image.h"
#include "vo.h"

struct vo_ohos_state {
    struct mp_log *log;
    OHNativeWindow *native_window;
    bool color_space_valid;
    OH_NativeBuffer_ColorSpace color_space;
    bool metadata_type_valid;
    OH_NativeBuffer_MetadataType metadata_type;
    bool static_metadata_valid;
    bool hdr_brightness_valid;
    float hdr_brightness;
    // True when the decoded stream carried HDR Vivid (CUVA) metadata.
    bool source_is_vivid;
};

static bool is_hdr(const struct pl_color_space *color)
{
    return color->transfer == PL_COLOR_TRC_PQ ||
           color->transfer == PL_COLOR_TRC_HLG;
}

static OH_NativeBuffer_ColorSpace get_color_space(
    const struct pl_color_space *color)
{
    if (color->transfer == PL_COLOR_TRC_PQ)
        return OH_COLORSPACE_DISPLAY_BT2020_PQ;
    if (color->transfer == PL_COLOR_TRC_HLG)
        return OH_COLORSPACE_DISPLAY_BT2020_HLG;
    return OH_COLORSPACE_DISPLAY_SRGB;
}

static OH_NativeBuffer_StaticMetadata get_static_metadata(
    const struct pl_color_space *color)
{
    struct pl_raw_primaries prim = color->hdr.prim;
    if (!pl_primaries_valid(&prim)) {
        const struct pl_raw_primaries *container =
            pl_raw_primaries_get(color->primaries);
        if (!container)
            container = pl_raw_primaries_get(PL_COLOR_PRIM_BT_2020);
        prim = *container;
    }

    return (OH_NativeBuffer_StaticMetadata){
        .smpte2086 = {
            .displayPrimaryRed = {prim.red.x, prim.red.y},
            .displayPrimaryGreen = {prim.green.x, prim.green.y},
            .displayPrimaryBlue = {prim.blue.x, prim.blue.y},
            .whitePoint = {prim.white.x, prim.white.y},
            .maxLuminance = color->hdr.max_luma,
            .minLuminance = color->hdr.min_luma,
        },
        .cta861 = {
            .maxContentLightLevel = color->hdr.max_cll,
            .maxFrameAverageLightLevel = color->hdr.max_fall,
        },
    };
}

// Decides which metadata type to advertise on the NativeWindow. HarmonyOS
// only ramps the panel to peak brightness when this is one of the HDR values,
// so it must reflect what is actually being pushed into the surface.
static OH_NativeBuffer_MetadataType resolve_metadata_type(
    struct vo *vo, const struct pl_color_space *color, bool hdr)
{
    // Announcing an HDR type while the surface holds an SDR buffer would tell
    // the compositor to expect PQ/HLG content that is not there, so the
    // forced modes below only apply once the output really is HDR.
    if (!hdr)
        return OH_VIDEO_NONE;

    switch (vo->opts->ohos_hdr_mode) {
    case OHOS_HDR_MODE_HDR10:
        return OH_VIDEO_HDR_HDR10;
    case OHOS_HDR_MODE_HLG:
        return OH_VIDEO_HDR_HLG;
    case OHOS_HDR_MODE_VIVID:
        // Used to map Dolby Vision onto HDR Vivid: libplacebo has already
        // applied the DV RPU, so the surface carries a finished PQ image and
        // only the signalling differs.
        return OH_VIDEO_HDR_VIVID;
    default:
        break;
    }

    if (vo->ohos->source_is_vivid)
        return OH_VIDEO_HDR_VIVID;
    return color->transfer == PL_COLOR_TRC_HLG ? OH_VIDEO_HDR_HLG
                                               : OH_VIDEO_HDR_HDR10;
}

static void set_output_color(struct vo *vo, struct pl_color_space color)
{
    struct vo_ohos_state *ctx = vo->ohos;

    pl_color_space_infer(&color);

    bool hdr = is_hdr(&color);
    OH_NativeBuffer_ColorSpace color_space = get_color_space(&color);
    OH_NativeBuffer_MetadataType metadata_type =
        resolve_metadata_type(vo, &color, hdr);
    float hdr_brightness = hdr ? 1.0f : 0.0f;

    if (!ctx->color_space_valid || ctx->color_space != color_space) {
        int ret = OH_NativeWindow_SetColorSpace(ctx->native_window, color_space);
        if (ret == 0) {
            ctx->color_space = color_space;
            ctx->color_space_valid = true;
            MP_VERBOSE(ctx, "NativeWindow output switched to %s\n",
                       hdr ? (color.transfer == PL_COLOR_TRC_HLG
                                  ? "BT.2020 HLG" : "BT.2020 PQ")
                           : "sRGB");
        } else {
            MP_WARN(ctx, "Failed to set NativeWindow color space: %d\n", ret);
        }
    }

    bool metadata_type_changed = !ctx->metadata_type_valid ||
                                 ctx->metadata_type != metadata_type;
    if (metadata_type_changed) {
        int ret = OH_NativeWindow_SetMetadataValue(
            ctx->native_window, OH_HDR_METADATA_TYPE, sizeof(metadata_type),
            (uint8_t *)&metadata_type);
        if (ret == 0) {
            ctx->metadata_type = metadata_type;
            ctx->metadata_type_valid = true;
        } else {
            MP_WARN(ctx, "Failed to set NativeWindow HDR metadata type: %d\n",
                    ret);
        }
    }

    if (hdr && (metadata_type_changed || !ctx->static_metadata_valid)) {
        OH_NativeBuffer_StaticMetadata metadata = get_static_metadata(&color);
        int ret = OH_NativeWindow_SetMetadataValue(
            ctx->native_window, OH_HDR_STATIC_METADATA, sizeof(metadata),
            (uint8_t *)&metadata);
        if (ret == 0) {
            ctx->static_metadata_valid = true;
        } else {
            MP_WARN(ctx, "Failed to set NativeWindow HDR static metadata: %d\n",
                    ret);
        }
    } else if (!hdr) {
        ctx->static_metadata_valid = false;
    }

    if (!ctx->hdr_brightness_valid || ctx->hdr_brightness != hdr_brightness) {
        int ret = OH_NativeWindow_NativeWindowHandleOpt(
            ctx->native_window, SET_HDR_WHITE_POINT_BRIGHTNESS, hdr_brightness);
        if (ret == 0) {
            ctx->hdr_brightness = hdr_brightness;
            ctx->hdr_brightness_valid = true;
        } else {
            MP_WARN(ctx, "Failed to set NativeWindow HDR brightness: %d\n", ret);
        }
    }
}

bool vo_ohos_init(struct vo *vo)
{
    vo->ohos = talloc_zero(vo, struct vo_ohos_state);
    struct vo_ohos_state *ctx = vo->ohos;

    *ctx = (struct vo_ohos_state){
        .log = mp_log_new(ctx, vo->log, "ohos"),
    };

    if (vo->opts->WinID == 0 || vo->opts->WinID == -1) {
        MP_FATAL(ctx, "Missing surface pointer\n");
        goto fail;
    }

    uint64_t surface = 0;
    memcpy(&surface, &vo->opts->WinID, sizeof(vo->opts->WinID));
    OH_NativeWindow_CreateNativeWindowFromSurfaceId(surface, &ctx->native_window);
    if (!ctx->native_window) {
        MP_FATAL(ctx, "Failed to create OHNativeWindow\n");
        goto fail;
    }

    return true;
fail:
    talloc_free(ctx);
    vo->ohos = NULL;
    return false;
}

void vo_ohos_uninit(struct vo *vo)
{
    struct vo_ohos_state *ctx = vo->ohos;
    if (!ctx)
        return;

    if (ctx->native_window)
        OH_NativeWindow_DestroyNativeWindow(ctx->native_window);

    talloc_free(ctx);
    vo->ohos = NULL;
}

OHNativeWindow *vo_ohos_native_window(struct vo *vo)
{
    struct vo_ohos_state *ctx = vo->ohos;
    return ctx->native_window;
}

bool vo_ohos_surface_size(struct vo *vo, int *out_w, int *out_h)
{
    struct vo_ohos_state *ctx = vo->ohos;

    int w = vo->opts->ohos_surface_size.w,
        h = vo->opts->ohos_surface_size.h;
    if (!w || !h)
        OH_NativeWindow_NativeWindowHandleOpt(ctx->native_window, GET_BUFFER_GEOMETRY, &h, &w);

    if (w <= 0 || h <= 0) {
        MP_ERR(ctx, "Failed to get height and width.\n");
        return false;
    }
    *out_w = w;
    *out_h = h;
    return true;
}

struct pl_color_space vo_ohos_preferred_csp(struct vo *vo)
{
    const struct pl_color_space color = vo_get_current_params(vo).color;

    if (color.transfer == PL_COLOR_TRC_PQ)
        return pl_color_space_hdr10;
    if (color.transfer == PL_COLOR_TRC_HLG)
        return pl_color_space_bt2020_hlg;
    return pl_color_space_srgb;
}

bool vo_ohos_set_color(struct vo *vo, struct mp_image_params *params)
{
    struct pl_color_space color = params ? params->color
                                         : pl_color_space_srgb;

    // --ohos-hdr-mode=no keeps the surface in SDR, which makes libplacebo tone
    // map the stream down instead of handing PQ/HLG to the compositor.
    bool allow_hdr = vo->opts->ohos_hdr_mode != OHOS_HDR_MODE_OFF;

    if (allow_hdr && is_hdr(&color)) {
        color.primaries = PL_COLOR_PRIM_BT_2020;
        pl_color_space_infer(&color);
    } else {
        color = pl_color_space_srgb;
    }

    if (params)
        params->color = color;

    struct pl_color_space window_color = color;
    if (is_hdr(&color)) {
        struct pl_color_space source = vo_get_current_params(vo).color;
        if (source.transfer == color.transfer)
            window_color.hdr = source.hdr;
    }
    set_output_color(vo, window_color);
    return true;
}

void vo_ohos_set_frame(struct vo *vo, struct mp_image *mpi)
{
    struct vo_ohos_state *ctx = vo->ohos;
    if (!ctx)
        return;

    bool was_vivid = ctx->source_is_vivid;
    ctx->source_is_vivid = false;

    for (int n = 0; mpi && n < mpi->num_ff_side_data; n++) {
        struct mp_ff_side_data *sd = &mpi->ff_side_data[n];
        if (!sd->buf)
            continue;

        if (sd->type == AV_FRAME_DATA_DYNAMIC_HDR_VIVID) {
            // Only the presence is recorded, never the payload. FFmpeg parses
            // CUVA but cannot serialise it back: there is no
            // av_dynamic_hdr_vivid_to_t35, here or upstream, and the raw T.35
            // bytes are destroyed at SEI parse time. So the payload
            // OH_HDR_DYNAMIC_METADATA wants cannot be reconstructed. Knowing
            // the source was Vivid still lets us advertise the right type.
            //
            // Forwarding HDR10+ in its place is not a substitute, and was
            // removed for being actively harmful: OHOS has no ST 2094-40
            // metadata type, so those bytes could only be sent under
            // OH_VIDEO_HDR_VIVID, which means a CUVA payload. A CUVA parser
            // validates only system_start_code in 0x01..0x07, while
            // av_dynamic_hdr_plus_to_t35 emits a headerless payload starting
            // with application_version = 0x01 — so it is accepted and
            // silently misparsed into garbage tone-curve parameters rather
            // than rejected.
            ctx->source_is_vivid = true;
        }
    }

    // The advertised metadata type depends on this, so force it to be resent.
    if (was_vivid != ctx->source_is_vivid)
        ctx->metadata_type_valid = false;
}

void vo_ohos_invalidate_color(struct vo *vo)
{
    struct vo_ohos_state *ctx = vo->ohos;

    ctx->color_space_valid = false;
    ctx->metadata_type_valid = false;
    ctx->static_metadata_valid = false;
    ctx->hdr_brightness_valid = false;
}
