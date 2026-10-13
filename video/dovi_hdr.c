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

#include <libavutil/dovi_meta.h>
#include <libavutil/frame.h>
#include <libavutil/rational.h>
#include <libavutil/version.h>
#include <libplacebo/colorspace.h>
#include <libplacebo/utils/dolbyvision.h>

#include "common/common.h"
#include "dovi_hdr.h"

// The extension blocks are only exported from this libavutil on.
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(59, 12, 100)

// Level 1 gives 12 bit PQ codes.
#define PQ_CODES 4095.0f

// Level 6 gives light levels in whole nits, up to PQ's 10000, and the minimum
// mastering luminance in units of 0.0001 nits.
#define MAX_NITS 10000
#define MIN_LUMA_UNITS 10000.0f

// The mastering display primaries level 9 names by index, in the order of
// dovi_tool's PREDEFINED_COLORSPACE_PRIMARIES. The indices past these name
// camera gamuts (ACES, S-Gamut), which no mastering display has.
static const enum pl_color_primaries l9_presets[] = {
    PL_COLOR_PRIM_DISPLAY_P3,   // DCI-P3 D65
    PL_COLOR_PRIM_BT_709,
    PL_COLOR_PRIM_BT_2020,
    PL_COLOR_PRIM_BT_601_525,   // SMPTE-C
    PL_COLOR_PRIM_BT_601_625,   // EBU
    PL_COLOR_PRIM_DCI_P3,
};

static float light_level(int nits)
{
    return nits <= MAX_NITS ? nits : 0;
}

static struct pl_cie_xy cie_xy(AVCIExy xy)
{
    return (struct pl_cie_xy) {av_q2d(xy.x), av_q2d(xy.y)};
}

// A chromaticity inside the xy diagram's triangle, as any display's is.
static bool realizable(struct pl_cie_xy xy)
{
    return xy.x >= 0 && xy.y > 0 && xy.x + xy.y <= 1;
}

static struct pl_raw_primaries l9_primaries(const AVDOVIDmLevel9 *l9)
{
    if (l9->source_primary_index < MP_ARRAY_SIZE(l9_presets))
        return *pl_raw_primaries_get(l9_presets[l9->source_primary_index]);

    // Any other index counts only with primaries in the block itself (index
    // 255); FFmpeg leaves them zero in a block too short to hold them.
    const AVColorPrimariesDesc *desc = &l9->source_display_primaries;
    struct pl_raw_primaries prim = {
        .red = cie_xy(desc->prim.r),
        .green = cie_xy(desc->prim.g),
        .blue = cie_xy(desc->prim.b),
        .white = cie_xy(desc->wp),
    };
    // pl_primaries_valid() puts the white point inside the primaries'
    // triangle, so it is realizable when they are.
    bool valid = realizable(prim.red) && realizable(prim.green) &&
                 realizable(prim.blue) && pl_primaries_valid(&prim);
    return valid ? prim : (struct pl_raw_primaries) {0};
}

static void fill_level6(struct pl_hdr_metadata *hdr, const AVDOVIDmLevel6 *l6)
{
    // The mastering luminance goes in as a pair, checked as libplacebo
    // checks the SEI's (pl_map_hdr_metadata).
    float min_luma = l6->min_luminance / MIN_LUMA_UNITS;
    float max_luma = light_level(l6->max_luminance);
    if (!hdr->max_luma && max_luma >= 5 && min_luma < max_luma) {
        hdr->min_luma = min_luma;
        hdr->max_luma = max_luma;
    }
    if (!hdr->max_cll)
        hdr->max_cll = light_level(l6->max_cll);
    if (!hdr->max_fall)
        hdr->max_fall = light_level(l6->max_fall);
}

static void fill_static_hdr(struct pl_hdr_metadata *hdr,
                            const AVDOVIMetadata *dovi)
{
    const AVDOVIDmData *l6 = av_dovi_find_level(dovi, 6);
    if (l6)
        fill_level6(hdr, &l6->l6);

    // Whole primaries or none: patching a partial set with another gamut's
    // coordinates would make up a gamut neither describes.
    const AVDOVIDmData *l9 = av_dovi_find_level(dovi, 9);
    if (l9 && !pl_primaries_valid(&hdr->prim)) {
        struct pl_raw_primaries prim = l9_primaries(&l9->l9);
        if (pl_primaries_valid(&prim))
            hdr->prim = prim;
    }
}

// Level 3 offsets are signed around 2048, one step being 1/2048 of the PQ
// range (dovi_tool's encoding of the XML's L1Offset).
static float l3_offset(int code)
{
    return (code - 2048) / 2048.0f;
}

bool mp_dovi_shot(const AVDOVIMetadata *dovi, struct mp_dovi_shot *shot)
{
    const AVDOVIDmData *l1 = av_dovi_find_level(dovi, 1);
    if (!l1 || !l1->l1.max_pq)
        return false;
    float min = l1->l1.min_pq / PQ_CODES;
    float avg = l1->l1.avg_pq / PQ_CODES;
    float peak = l1->l1.max_pq / PQ_CODES;
    float max = peak;
    const AVDOVIDmData *l3 = av_dovi_find_level(dovi, 3);
    if (l3) {
        min += l3_offset(l3->l3.min_pq_offset);
        avg += l3_offset(l3->l3.avg_pq_offset);
        max += l3_offset(l3->l3.max_pq_offset);
    }
    shot->max = MPMIN(MPMAX(max, peak), 1);
    shot->min = MPCLAMP(min, 0, shot->max);
    // A picture with a peak is not black on average, yet level 1 may round its
    // average to black and a mid tone offset may take it there: the lowest
    // code keeps it the darkest average there is. libplacebo needs that too,
    // as it reads a zero average as no dynamic metadata at all and would tone
    // map by the static peak instead.
    shot->avg = MPCLAMP(avg, MPMAX(shot->min, 1 / PQ_CODES), shot->max);
    return true;
}

#else

static void fill_static_hdr(struct pl_hdr_metadata *hdr,
                            const AVDOVIMetadata *dovi)
{
}

bool mp_dovi_shot(const AVDOVIMetadata *dovi, struct mp_dovi_shot *shot)
{
    return false;
}

#endif

void mp_dovi_map_hdr(struct pl_hdr_metadata *hdr, const AVFrame *frame)
{
    const AVFrameSideData *sd =
        av_frame_get_side_data(frame, AV_FRAME_DATA_DOVI_METADATA);
    const AVDOVIMetadata *dovi = sd ? (const AVDOVIMetadata *)sd->buf->data
                                    : NULL;
    if (dovi)
        fill_static_hdr(hdr, dovi);

    sd = av_frame_get_side_data(frame, AV_FRAME_DATA_DOVI_RPU_BUFFER);
    if (sd)
        pl_hdr_metadata_from_dovi_rpu(hdr, sd->buf->data, sd->buf->size);

    struct mp_dovi_shot shot;
    if (dovi && hdr->max_pq_y && mp_dovi_shot(dovi, &shot)) {
        hdr->max_pq_y = shot.max;
        hdr->avg_pq_y = shot.avg;
    }
}
