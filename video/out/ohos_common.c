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

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <libplacebo/shaders/colorspace.h>
#include <libplacebo/tone_mapping.h>

#include "ohos_common.h"
#include "ohos_hdr_vivid.h"
#include "common/msg.h"
#include "video/mp_image.h"
#include "vo.h"

struct vo_ohos_state {
    struct mp_log *log;
    OHNativeWindow *native_window;

    // The frame being drawn, captured by vo_ohos_set_frame(), with the
    // dynamic metadata of its stream.
    struct mp_image_params source;
    const struct pl_color_map_params *color_map;
    struct ohos_vivid_tracker vivid;
    // The tone mapping from the frame to the current render target.
    struct pl_tone_map_params tone;
    // The brightest the stream has shown so far (PQ): the peaks its frames
    // carry (Dolby Vision, HDR Vivid) and libplacebo has measured, see
    // vo_ohos_set_frame() and vo_ohos_set_measured_peak().
    float observed_peak;
    // The stream's Dolby Vision RPU as last logged, see log_dovi().
    char dovi_desc[128];

    // What the NativeWindow was last told, so that only changes are sent.
    bool color_space_valid;
    OH_NativeBuffer_ColorSpace color_space;
    bool metadata_type_valid;
    OH_NativeBuffer_MetadataType metadata_type;
    bool static_metadata_valid;
    OH_NativeBuffer_StaticMetadata static_metadata;
    uint8_t dynamic_metadata[OHOS_VIVID_MAX_SIZE];
    size_t dynamic_metadata_size;
    bool hdr_brightness_valid;
    float hdr_brightness;
};

static const char *const dynamic_names[] = {
    [OHOS_DYNAMIC_HDR_NONE]  = "none",
    [OHOS_DYNAMIC_HDR_DOVI]  = "Dolby Vision",
    [OHOS_DYNAMIC_HDR_VIVID] = "HDR Vivid",
    [OHOS_DYNAMIC_HDR_PLUS]  = "HDR10+",
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

static const char *metadata_type_name(OH_NativeBuffer_MetadataType type)
{
    switch (type) {
    case OH_VIDEO_HDR_HLG:   return "HLG";
    case OH_VIDEO_HDR_HDR10: return "HDR10";
    case OH_VIDEO_HDR_VIVID: return "HDR Vivid";
    default:                 return "none";
    }
}

// The output as the window's metadata describes it: the render target, with
// the light levels the picture actually reaches. libplacebo never raises a
// stream's light levels unless asked to expand it (--inverse-tone-mapping):
// it compresses what is brighter than the target into it and passes the rest
// untouched. So a stream no brighter than the target, by its metadata and by
// what it has shown so far, keeps its own light levels and is described
// exactly as without a target peak: a target peak (--target-peak) above it
// never makes the system show it any brighter. A brighter stream gets the
// target's, as the colourspace hint set them. (An HLG target carries the
// stream's own light levels already.)
static struct pl_color_space get_output_color(
    const struct vo_ohos_state *ctx, const struct pl_color_space *target)
{
    struct pl_color_space out = *target;
    if (ctx->color_map && ctx->color_map->inverse_tone_mapping)
        return out;

    struct pl_color_space source = ctx->source.color;
    pl_color_space_infer(&source);
    const struct pl_hdr_metadata *hdr = &source.hdr;
    float peak = MPMAX(hdr->max_cll ? hdr->max_cll : hdr->max_luma,
                       pl_hdr_rescale(PL_HDR_PQ, PL_HDR_NITS,
                                      ctx->observed_peak));
    if (peak > target->hdr.max_luma)
        return out;

    out.hdr.max_luma = hdr->max_luma;
    out.hdr.max_cll = peak;
    out.hdr.max_fall = hdr->max_fall;
    return out;
}

// The render service sizes the layer's HDR brightness from MaxCLL, so it is
// never left at 0 (which grants no headroom at all): without a content light
// level the peak stands in, as libplacebo itself assumes.
static OH_NativeBuffer_StaticMetadata get_static_metadata(
    const struct pl_color_space *color)
{
    const struct pl_raw_primaries *prim = &color->hdr.prim;
    float max_cll = color->hdr.max_cll ? color->hdr.max_cll
                                       : color->hdr.max_luma;
    return (OH_NativeBuffer_StaticMetadata){
        .smpte2086 = {
            .displayPrimaryRed = {prim->red.x, prim->red.y},
            .displayPrimaryGreen = {prim->green.x, prim->green.y},
            .displayPrimaryBlue = {prim->blue.x, prim->blue.y},
            .whitePoint = {prim->white.x, prim->white.y},
            .maxLuminance = color->hdr.max_luma,
            .minLuminance = color->hdr.min_luma,
        },
        .cta861 = {
            .maxContentLightLevel = max_cll,
            .maxFrameAverageLightLevel = color->hdr.max_fall <= max_cll
                                             ? color->hdr.max_fall : 0,
        },
    };
}

// HarmonyOS only ramps the panel to its HDR brightness for the HDR types, and
// HDR Vivid is its only format with dynamic metadata: Dolby Vision and HDR10+
// travel in it too, their metadata converted.
static OH_NativeBuffer_MetadataType get_metadata_type(
    struct vo *vo, const struct pl_color_space *color)
{
    if (!is_hdr(color))
        return OH_VIDEO_NONE;
    if (vo->ohos->vivid.dynamic != OHOS_DYNAMIC_HDR_NONE &&
        vo->opts->ohos_hdr_mode == OHOS_HDR_MODE_AUTO)
        return OH_VIDEO_HDR_VIVID;
    return color->transfer == PL_COLOR_TRC_HLG ? OH_VIDEO_HDR_HLG
                                               : OH_VIDEO_HDR_HDR10;
}

// HDR Vivid metadata describing the picture as it leaves the renderer. A
// stream that passes through untouched keeps its own (curve parameters
// included); otherwise it is described by its statistics, mapped through the
// tone curve when libplacebo compresses it into the target.
//
// A surface keeps the last dynamic metadata it was given (it cannot be
// cleared), so an HDR10 picture gets one matching its static metadata, in
// place of whatever an earlier HDR Vivid picture left behind.
static size_t get_dynamic_metadata(struct vo_ohos_state *ctx,
                                   OH_NativeBuffer_MetadataType type,
                                   const struct pl_color_space *target,
                                   uint8_t *buf, size_t size)
{
    const struct ohos_vivid_tracker *vivid = &ctx->vivid;
    struct ohos_vivid_stats stats;
    if (type == OH_VIDEO_HDR_VIVID) {
        if (vivid->native_size && pl_tone_map_params_noop(&ctx->tone)) {
            memcpy(buf, vivid->native, vivid->native_size);
            return vivid->native_size;
        }
        stats = vivid->stats;
        ohos_vivid_tone_map(&stats, &ctx->tone);
    } else {
        ohos_vivid_stats_from_static(&stats, &target->hdr);
    }

    AVDynamicHDRVivid md;
    ohos_vivid_from_stats(&md, &stats);
    return ohos_vivid_write(&md, buf, size);
}

static void log_dynamic_metadata(struct vo_ohos_state *ctx,
                                 const uint8_t *md, size_t size)
{
    const struct ohos_vivid_tracker *vivid = &ctx->vivid;
    // The four 12 bit statistics following the 8 bit system_start_code, which
    // any payload (8 bytes at least) holds.
    int min = md[1] << 4 | md[2] >> 4;
    int avg = (md[2] & 0xf) << 8 | md[3];
    int var = md[4] << 4 | md[5] >> 4;
    int max = (md[5] & 0xf) << 8 | md[6];
    // When libplacebo compresses the picture, the average and peak it tone
    // maps by as well: which luminance (Dolby Vision level 1 with level 3,
    // say) reached the tone curve.
    char tone[64] = "";
    if (!pl_tone_map_params_noop(&ctx->tone)) {
        snprintf(tone, sizeof(tone), ", tone mapped from avg %ld max %ld",
                 lrintf(ctx->tone.input_avg * 4095),
                 lrintf(ctx->tone.input_max * 4095));
    }
    MP_DBG(ctx, "HDR Vivid metadata (%s%s): maxRGB min %d avg %d var %d max %d "
           "of 4095, %zu bytes%s\n", dynamic_names[vivid->dynamic],
           size == vivid->native_size && !memcmp(md, vivid->native, size)
               ? ", native" : "",
           min, avg, var, max, size, tone);
}

static bool set_window_metadata(struct vo_ohos_state *ctx,
                                OH_NativeBuffer_MetadataKey key,
                                const void *value, size_t size,
                                const char *name)
{
    int ret = OH_NativeWindow_SetMetadataValue(ctx->native_window, key, size,
                                               (uint8_t *)value);
    if (ret)
        MP_WARN(ctx, "Failed to set NativeWindow %s: %d\n", name, ret);
    return !ret;
}

static void send_static_metadata(struct vo_ohos_state *ctx,
                                 const struct pl_color_space *color)
{
    if (!is_hdr(color)) {
        ctx->static_metadata_valid = false;
        return;
    }

    OH_NativeBuffer_StaticMetadata md = get_static_metadata(color);
    if (ctx->static_metadata_valid &&
        !memcmp(&md, &ctx->static_metadata, sizeof(md)))
        return;

    ctx->static_metadata = md;
    ctx->static_metadata_valid = set_window_metadata(ctx,
        OH_HDR_STATIC_METADATA, &md, sizeof(md), "HDR static metadata");
    const OH_NativeBuffer_Smpte2086 *m = &md.smpte2086;
    MP_VERBOSE(ctx, "NativeWindow HDR static metadata: mastering "
               "%.4f-%.0f nits, MaxCLL %.0f, MaxFALL %.0f, primaries "
               "R %.4f,%.4f G %.4f,%.4f B %.4f,%.4f W %.4f,%.4f\n",
               m->minLuminance, m->maxLuminance,
               md.cta861.maxContentLightLevel,
               md.cta861.maxFrameAverageLightLevel,
               m->displayPrimaryRed.x, m->displayPrimaryRed.y,
               m->displayPrimaryGreen.x, m->displayPrimaryGreen.y,
               m->displayPrimaryBlue.x, m->displayPrimaryBlue.y,
               m->whitePoint.x, m->whitePoint.y);
}

static void send_dynamic_metadata(struct vo_ohos_state *ctx,
                                  OH_NativeBuffer_MetadataType type,
                                  const struct pl_color_space *color)
{
    // HLG and SDR carry none.
    if (type != OH_VIDEO_HDR_VIVID && type != OH_VIDEO_HDR_HDR10)
        return;

    uint8_t md[OHOS_VIVID_MAX_SIZE];
    size_t size = get_dynamic_metadata(ctx, type, color, md, sizeof(md));
    if (!size || (size == ctx->dynamic_metadata_size &&
                  !memcmp(md, ctx->dynamic_metadata, size)))
        return;
    if (!set_window_metadata(ctx, OH_HDR_DYNAMIC_METADATA, md, size,
                             "HDR dynamic metadata"))
        return;

    memcpy(ctx->dynamic_metadata, md, size);
    ctx->dynamic_metadata_size = size;
    log_dynamic_metadata(ctx, md, size);
}

static void update_window(struct vo *vo, const struct pl_color_space *color)
{
    struct vo_ohos_state *ctx = vo->ohos;
    bool hdr = is_hdr(color);

    OH_NativeBuffer_ColorSpace color_space = get_color_space(color);
    if (!ctx->color_space_valid || ctx->color_space != color_space) {
        int ret = OH_NativeWindow_SetColorSpace(ctx->native_window, color_space);
        ctx->color_space = color_space;
        ctx->color_space_valid = !ret;
        if (ret) {
            MP_WARN(ctx, "Failed to set NativeWindow color space: %d\n", ret);
        } else {
            MP_VERBOSE(ctx, "NativeWindow output switched to %s\n",
                       !hdr ? "sRGB" : color->transfer == PL_COLOR_TRC_HLG
                                           ? "BT.2020 HLG" : "BT.2020 PQ");
        }
    }

    OH_NativeBuffer_MetadataType type = get_metadata_type(vo, color);
    if (!ctx->metadata_type_valid || ctx->metadata_type != type) {
        ctx->metadata_type = type;
        ctx->metadata_type_valid = set_window_metadata(ctx,
            OH_HDR_METADATA_TYPE, &type, sizeof(type), "HDR metadata type");
        // The dynamic metadata is tied to the type; send it afresh.
        ctx->dynamic_metadata_size = 0;
        MP_VERBOSE(ctx, "NativeWindow HDR type %s (dynamic metadata: %s)\n",
                   metadata_type_name(type), dynamic_names[ctx->vivid.dynamic]);
    }

    send_static_metadata(ctx, color);
    send_dynamic_metadata(ctx, type, color);

    float hdr_brightness = hdr ? 1.0f : 0.0f;
    if (!ctx->hdr_brightness_valid || ctx->hdr_brightness != hdr_brightness) {
        int ret = OH_NativeWindow_NativeWindowHandleOpt(
            ctx->native_window, SET_HDR_WHITE_POINT_BRIGHTNESS, hdr_brightness);
        ctx->hdr_brightness = hdr_brightness;
        ctx->hdr_brightness_valid = !ret;
        if (ret)
            MP_WARN(ctx, "Failed to set NativeWindow HDR brightness: %d\n", ret);
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
    struct vo_ohos_state *ctx = vo->ohos;
    struct pl_color_space color = params ? params->color
                                         : pl_color_space_srgb;

    // --ohos-hdr-mode=no keeps the surface in SDR, which makes libplacebo tone
    // map the stream down instead of handing PQ/HLG to the compositor.
    if (vo->opts->ohos_hdr_mode != OHOS_HDR_MODE_OFF && is_hdr(&color)) {
        color.primaries = PL_COLOR_PRIM_BT_2020;
        // HLG is scene referred and the system adapts it to the panel, so no
        // display peak (--target-peak) is baked into it here.
        if (color.transfer == PL_COLOR_TRC_HLG)
            color.hdr = ctx->source.color.hdr;
        pl_color_space_infer(&color);

        // The target's light levels are left as the colorspace hint set them,
        // the target peak (--target-peak) included: libplacebo compresses
        // only what is brighter, so lowering them to a stream's metadata
        // would compress a stream brighter than its metadata says.
        ohos_vivid_tone_map_params(&ctx->tone, ctx->color_map,
                                   &ctx->source.color, &color);
    } else {
        color = pl_color_space_srgb;
    }

    // This becomes the render target; the window metadata describes the
    // picture rendered into it, whether or not libplacebo tone maps.
    if (params)
        params->color = color;
    struct pl_color_space output = get_output_color(ctx, &color);
    update_window(vo, &output);
    return true;
}

// Which Dolby Vision metadata levels the stream carries, since what reaches
// HDR Vivid depends on it, and its raw level 3 offsets, which move the
// statistics HDR Vivid gets. Logged once, then on every change.
static void log_dovi(struct vo_ohos_state *ctx, const struct mp_image *mpi)
{
    char desc[sizeof(ctx->dovi_desc)];
    ohos_dovi_describe(mpi, desc, sizeof(desc));
    if (!desc[0] || !strcmp(desc, ctx->dovi_desc))
        return;
    MP_VERBOSE(ctx, "Dolby Vision RPU levels: %s\n", desc);
    memcpy(ctx->dovi_desc, desc, sizeof(desc));
}

void vo_ohos_set_frame(struct vo *vo, const struct vo_frame *frame,
                       const struct pl_color_map_params *color_map)
{
    struct vo_ohos_state *ctx = vo->ohos;
    struct mp_image *mpi = frame->current;
    if (!ctx || !mpi)
        return;

    // A new format or size is a new stream as much as a seek is.
    if (!mp_image_params_static_equal(&ctx->source, &mpi->params)) {
        ohos_vivid_tracker_reset(&ctx->vivid);
        ctx->observed_peak = 0;
        ctx->dovi_desc[0] = '\0';
    }
    ctx->source = mpi->params;
    ctx->color_map = color_map;
    // Dolby Vision and HDR Vivid carry each frame's own peak, which libplacebo
    // maps them by instead of measuring them.
    const struct pl_hdr_metadata *hdr = &mpi->params.color.hdr;
    if (pl_hdr_metadata_contains(hdr, PL_HDR_METADATA_CIE_Y))
        ctx->observed_peak = MPMAX(ctx->observed_peak, hdr->max_pq_y);
    ohos_vivid_tracker_update(&ctx->vivid, mpi, frame->frame_id);
    log_dovi(ctx, mpi);
}

// libplacebo measures afresh after a seek or for a new file as well
// (pl_renderer_flush_cache()).
void vo_ohos_reset(struct vo *vo)
{
    struct vo_ohos_state *ctx = vo->ohos;
    if (!ctx)
        return;
    ohos_vivid_tracker_reset(&ctx->vivid);
    ctx->observed_peak = 0;
}

void vo_ohos_set_measured_peak(struct vo *vo, float max_pq_y)
{
    struct vo_ohos_state *ctx = vo->ohos;
    if (ctx)
        ctx->observed_peak = MPMAX(ctx->observed_peak, max_pq_y);
}

void vo_ohos_invalidate_color(struct vo *vo)
{
    struct vo_ohos_state *ctx = vo->ohos;

    ctx->color_space_valid = false;
    ctx->metadata_type_valid = false;
    ctx->static_metadata_valid = false;
    ctx->dynamic_metadata_size = 0;
    ctx->hdr_brightness_valid = false;
}
