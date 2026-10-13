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
#include <string.h>

#include <libavutil/attributes.h>
#include <libavutil/dovi_meta.h>
#include <libavutil/frame.h>
#include <libavutil/hdr_dynamic_metadata.h>
#include <libplacebo/colorspace.h>
#include <libplacebo/shaders/colorspace.h>
#include <libplacebo/tone_mapping.h>

#include "common/common.h"
#include "ohos_hdr_vivid.h"
#include "video/dovi_hdr.h"
#include "video/mp_image.h"

// The denominator of the T/UWA 005.1 maxRGB statistics, as FFmpeg's parser
// uses it.
#define MAXRGB_DEN 4095

static const void *find_side_data(const struct mp_image *mpi, int type)
{
    for (int n = 0; n < mpi->num_ff_side_data; n++) {
        if (mpi->ff_side_data[n].type == type)
            return mpi->ff_side_data[n].buf->data;
    }
    return NULL;
}

static float pq(float nits)
{
    return pl_hdr_rescale(PL_HDR_NITS, PL_HDR_PQ, nits);
}

// Spread of a distribution known only by its range and mean: the standard
// deviation of the triangular distribution with that range and mean.
static float spread_from_range(float min, float avg, float max)
{
    float mode = MPCLAMP(3 * avg - min - max, min, max);
    float var = (min * min + max * max + mode * mode -
                 min * max - min * mode - max * mode) / 18;
    return sqrtf(MPMAX(var, 0));
}

static bool stats_from_vivid(const AVDynamicHDRVivid *vivid,
                             struct ohos_vivid_stats *st)
{
    if (vivid->num_windows < 1)
        return false;
    const AVHDRVividColorTransformParams *p = &vivid->params[0];
    *st = (struct ohos_vivid_stats) {
        .min = av_q2d(p->minimum_maxrgb),
        .avg = av_q2d(p->average_maxrgb),
        .var = av_q2d(p->variance_maxrgb),
        .max = av_q2d(p->maximum_maxrgb),
    };
    return st->max > 0;
}

// Dolby Vision level 1 holds the minimum, average and maximum of the picture
// as 12 bit PQ codes, exactly HDR Vivid's quantisation, and level 3 turns them
// into CM v4.0's analysis: the same luminance libplacebo tone maps the frame
// by (mp_dovi_shot()). Its peak not dropping below level 1's matters here as
// well: the system sizes the layer's HDR brightness from the peak, and
// reporting less than the picture holds would squeeze its highlights towards
// SDR white.
static bool stats_from_dovi(const AVDOVIMetadata *dovi,
                            struct ohos_vivid_stats *st)
{
    struct mp_dovi_shot shot;
    if (!mp_dovi_shot(dovi, &shot))
        return false;
    *st = (struct ohos_vivid_stats) {
        .min = shot.min,
        .avg = shot.avg,
        .var = spread_from_range(shot.min, shot.avg, shot.max),
        .max = shot.max,
    };
    return true;
}

// HDR10+ describes the maxRGB distribution of the scene through linear light
// percentiles. Two of its slots are not percentiles: 5 carries the 99.99th
// one and 10 the share of pixels under 100 nits, while 99 stands for the
// 99.98th. Treating the picture as uniformly distributed between the true
// percentiles yields the PQ domain mean and spread HDR Vivid wants.
static bool stats_from_hdr10plus(const AVDynamicHDRPlus *plus,
                                 struct ohos_vivid_stats *st)
{
    // Later versions may mean something else; libplacebo ignores them too.
    if (plus->num_windows < 1 || plus->application_version >= 2)
        return false;
    const AVHDRPlusColorTransformParams *p = &plus->params[0];
    int n = MPMIN(p->num_distribution_maxrgb_percentiles,
                  MP_ARRAY_SIZE(p->distribution_maxrgb));

    float xs[MP_ARRAY_SIZE(p->distribution_maxrgb)];
    float ps[MP_ARRAY_SIZE(p->distribution_maxrgb)];
    float peak = 0, hist_max = 0;
    int m = 0;
    for (int i = 0; i < 3; i++)
        peak = MPMAX(peak, av_q2d(p->maxscl[i]));
    for (int i = 0; i < n; i++) {
        int percentage = p->distribution_maxrgb[i].percentage;
        float x = av_q2d(p->distribution_maxrgb[i].percentile);
        if (percentage == 10)
            continue;
        hist_max = MPMAX(hist_max, x);
        if (percentage == 5)
            continue;
        xs[m] = pq(x * 10000);
        ps[m++] = percentage == 99 ? 0.9998f : percentage / 100.0f;
    }
    // Without MaxSCL the brightest histogram value stands in, as in libplacebo,
    // except that the 10% slot holds a share of pixels, not a light level.
    if (peak <= 0)
        peak = hist_max;
    if (peak <= 0)
        return false;
    st->max = pq(peak * 10000);

    if (m < 2) {
        st->min = 0;
        st->avg = MPMIN(pq(av_q2d(p->average_maxrgb) * 10000), st->max);
        st->var = spread_from_range(st->min, st->avg, st->max);
        return true;
    }

    float sum = 0, sum_sq = 0, last_p = 0;
    float last_x = MPMIN(xs[0], st->max);
    st->min = last_x;
    for (int i = 0; i <= m; i++) {
        float x = i < m ? xs[i] : st->max;
        float pc = i < m ? ps[i] : 1.0f;
        x = MPCLAMP(x, last_x, st->max);
        float w = MPMAX(pc - last_p, 0);
        sum += w * (last_x + x) / 2;
        sum_sq += w * (last_x * last_x + last_x * x + x * x) / 3;
        last_x = x;
        last_p = MPMAX(pc, last_p);
    }
    st->avg = sum;
    st->var = sqrtf(MPMAX(sum_sq - sum * sum, 0));
    return true;
}

enum ohos_dynamic_hdr ohos_vivid_probe(const struct mp_image *mpi,
                                       struct ohos_vivid_stats *stats,
                                       const AVDynamicHDRVivid **native)
{
    *native = NULL;
    const AVDOVIMetadata *dovi =
        find_side_data(mpi, AV_FRAME_DATA_DOVI_METADATA);
    if (dovi && stats_from_dovi(dovi, stats))
        return OHOS_DYNAMIC_HDR_DOVI;

    const AVDynamicHDRVivid *vivid =
        find_side_data(mpi, AV_FRAME_DATA_DYNAMIC_HDR_VIVID);
    if (vivid && stats_from_vivid(vivid, stats)) {
        *native = vivid;
        return OHOS_DYNAMIC_HDR_VIVID;
    }

    const AVDynamicHDRPlus *plus =
        find_side_data(mpi, AV_FRAME_DATA_DYNAMIC_HDR_PLUS);
    if (plus && stats_from_hdr10plus(plus, stats))
        return OHOS_DYNAMIC_HDR_PLUS;

    return OHOS_DYNAMIC_HDR_NONE;
}

void ohos_dovi_describe(const struct mp_image *mpi, char *buf, size_t size)
{
    buf[0] = '\0';
    const AVDOVIMetadata *dovi =
        find_side_data(mpi, AV_FRAME_DATA_DOVI_METADATA);
    if (!dovi)
        return;

    int count[UINT8_MAX + 1] = {0};
    for (int i = 0; i < dovi->num_ext_blocks; i++)
        count[av_dovi_get_ext(dovi, i)->level]++;

    for (int level = 0; level <= UINT8_MAX; level++) {
        if (!count[level])
            continue;
        mp_snprintf_cat(buf, size, "%s%d", buf[0] ? " " : "", level);
        if (count[level] > 1)
            mp_snprintf_cat(buf, size, "x%d", count[level]);
        if (level == 3) {
            const AVDOVIDmLevel3 *l3 = &av_dovi_find_level(dovi, 3)->l3;
            mp_snprintf_cat(buf, size, "(min %d max %d avg %d)",
                            l3->min_pq_offset, l3->max_pq_offset,
                            l3->avg_pq_offset);
        }
    }
}

void ohos_vivid_tracker_reset(struct ohos_vivid_tracker *t)
{
    t->reset = true;
}

void ohos_vivid_tracker_update(struct ohos_vivid_tracker *t,
                               const struct mp_image *mpi, uint64_t frame_id)
{
    if (frame_id > t->frame_id) {
        if (t->reset)
            *t = (struct ohos_vivid_tracker){0};
        t->frame_id = frame_id;
    }

    struct ohos_vivid_stats stats;
    const AVDynamicHDRVivid *native;
    enum ohos_dynamic_hdr dynamic = ohos_vivid_probe(mpi, &stats, &native);
    if (dynamic == OHOS_DYNAMIC_HDR_NONE)
        return;
    t->dynamic = dynamic;
    t->stats = stats;
    t->native_size = native ? ohos_vivid_write(native, t->native,
                                               sizeof(t->native))
                            : 0;
}

void ohos_vivid_stats_from_static(struct ohos_vivid_stats *st,
                                  const struct pl_hdr_metadata *hdr)
{
    float max = hdr->max_cll ? hdr->max_cll : hdr->max_luma;
    // MaxFALL bounds the frame average from above; without it, assume diffuse
    // white (BT.2408) as the typical level.
    float avg = hdr->max_fall > 0 && hdr->max_fall <= max ? hdr->max_fall
                                                          : MPMIN(203, max);
    st->min = pq(hdr->min_luma);
    st->max = pq(max);
    st->avg = MPCLAMP(pq(avg), st->min, st->max);
    st->var = spread_from_range(st->min, st->avg, st->max);
}

void ohos_vivid_tone_map_params(struct pl_tone_map_params *out,
                                const struct pl_color_map_params *params,
                                const struct pl_color_space *src_csp,
                                const struct pl_color_space *dst_csp)
{
    if (!params)
        params = &pl_color_map_default_params;
    struct pl_color_space src = *src_csp, dst = *dst_csp;
    pl_color_space_infer_map(&src, &dst);
    // Between equal spaces libplacebo skips colour mapping altogether.
    bool same = pl_color_space_equal(&src, &dst);

    *out = (struct pl_tone_map_params) {
        .function       = params->tone_mapping_function
                              ? params->tone_mapping_function
                              : &pl_tone_map_clip,
        .constants      = params->tone_constants,
        .input_scaling  = PL_HDR_PQ,
        .output_scaling = PL_HDR_PQ,
        .hdr            = src.hdr,
    };
AV_NOWARN_DEPRECATED(
    out->param = params->tone_mapping_param;
)
    pl_color_space_nominal_luma_ex(pl_nominal_luma_params(
        .color      = &src,
        .metadata   = params->metadata,
        .scaling    = out->input_scaling,
        .out_min    = &out->input_min,
        .out_max    = &out->input_max,
        .out_avg    = &out->input_avg,
    ));
    pl_color_space_nominal_luma_ex(pl_nominal_luma_params(
        .color      = &dst,
        .metadata   = PL_HDR_METADATA_HDR10,
        .scaling    = out->output_scaling,
        .out_min    = &out->output_min,
        .out_max    = &out->output_max,
    ));
    pl_tone_map_params_infer(out);

    // Round sufficiently similar values, and never exceed the source unless
    // asked to expand it: a scene dimmer than the target passes untouched.
    if (fabsf(out->input_max - out->output_max) < 1e-6)
        out->output_max = out->input_max;
    if (fabsf(out->input_min - out->output_min) < 1e-6)
        out->output_min = out->input_min;
    if (!params->inverse_tone_mapping)
        out->output_max = MPMIN(out->output_max, out->input_max);
    if (same) {
        out->output_min = out->input_min;
        out->output_max = out->input_max;
    }
}

void ohos_vivid_tone_map(struct ohos_vivid_stats *st,
                         const struct pl_tone_map_params *tone)
{
    if (pl_tone_map_params_noop(tone))
        return;

    // Carry the spread over with the curve's slope around the mean.
    float lo = MPMAX(st->avg - st->var, st->min);
    float hi = MPMIN(st->avg + st->var, st->max);
    if (hi > lo) {
        st->var *= (pl_tone_map_sample(hi, tone) -
                    pl_tone_map_sample(lo, tone)) / (hi - lo);
    }
    st->min = pl_tone_map_sample(st->min, tone);
    st->avg = pl_tone_map_sample(st->avg, tone);
    st->max = pl_tone_map_sample(st->max, tone);
}

static AVRational maxrgb_code(float value)
{
    return av_make_q(lrintf(MPCLAMP(value, 0, 1) * MAXRGB_DEN), MAXRGB_DEN);
}

void ohos_vivid_from_stats(AVDynamicHDRVivid *out,
                           const struct ohos_vivid_stats *st)
{
    float max = MPCLAMP(st->max, 0, 1);
    float min = MPCLAMP(st->min, 0, max);
    float avg = MPCLAMP(st->avg, min, max);

    *out = (AVDynamicHDRVivid) {
        .system_start_code = 0x01,
        .num_windows = 1,
    };
    out->params[0] = (AVHDRVividColorTransformParams) {
        .minimum_maxrgb  = maxrgb_code(min),
        .average_maxrgb  = maxrgb_code(avg),
        .variance_maxrgb = maxrgb_code(st->var),
        .maximum_maxrgb  = maxrgb_code(max),
    };
}

struct bit_writer {
    uint8_t *buf;
    size_t size;
    size_t pos;
    bool overflow;
};

static void put_bits(struct bit_writer *w, int bits, unsigned value)
{
    while (bits--) {
        if (w->pos >= w->size * 8) {
            w->overflow = true;
            return;
        }
        if ((value >> bits) & 1)
            w->buf[w->pos / 8] |= 0x80 >> (w->pos % 8);
        w->pos++;
    }
}

// Quantises a field to its bitstream code. Fields parsed by FFmpeg already
// carry the code over the spec's denominator, so this round trips exactly.
static void put_q(struct bit_writer *w, int bits, AVRational q, int den)
{
    double code = q.den ? round(av_q2d(q) * den) : 0;
    put_bits(w, bits, MPCLAMP(code, 0, (1 << bits) - 1));
}

static void put_tone_mapping(struct bit_writer *w,
                             const AVHDRVividColorToneMappingParams *tm)
{
    put_q(w, 12, tm->targeted_system_display_maximum_luminance, 4095);
    put_bits(w, 1, !!tm->base_enable_flag);
    if (tm->base_enable_flag) {
        put_q(w, 14, tm->base_param_m_p, 16383);
        put_q(w, 6, tm->base_param_m_m, 10);
        put_q(w, 10, tm->base_param_m_a, 1023);
        put_q(w, 10, tm->base_param_m_b, 1023);
        put_q(w, 6, tm->base_param_m_n, 10);
        put_bits(w, 2, tm->base_param_k1);
        put_bits(w, 2, tm->base_param_k2);
        put_bits(w, 4, tm->base_param_k3);
        put_bits(w, 3, tm->base_param_Delta_enable_mode);
        put_q(w, 7, tm->base_param_Delta, 127);
    }
    put_bits(w, 1, !!tm->three_Spline_enable_flag);
    if (tm->three_Spline_enable_flag) {
        int num = MPCLAMP(tm->three_Spline_num, 1, 2);
        put_bits(w, 1, num - 1);
        for (int i = 0; i < num; i++) {
            const AVHDRVivid3SplineParams *s = &tm->three_spline[i];
            put_bits(w, 2, s->th_mode);
            if (s->th_mode == 0 || s->th_mode == 2)
                put_q(w, 8, s->th_enable_mb, 255);
            put_q(w, 12, s->th_enable, 4095);
            put_q(w, 10, s->th_delta1, 1023);
            put_q(w, 10, s->th_delta2, 1023);
            put_q(w, 8, s->enable_strength, 255);
        }
    }
}

// Writes T/UWA 005.1 dynamic_metadata(), the body of the ITU-T T.35 payload
// after its country and provider codes, which is what HarmonyOS takes (its own
// samples start at system_start_code). Mirrors
// ff_parse_itu_t_t35_to_dynamic_hdr_vivid(), which reads exactly one window
// for system_start_code 0x01..0x07 and nothing for other versions.
size_t ohos_vivid_write(const AVDynamicHDRVivid *vivid, uint8_t *buf,
                        size_t size)
{
    if (vivid->system_start_code < 0x01 || vivid->system_start_code > 0x07 ||
        vivid->num_windows < 1)
        return 0;

    memset(buf, 0, size);
    struct bit_writer w = {.buf = buf, .size = size};

    const AVHDRVividColorTransformParams *p = &vivid->params[0];
    put_bits(&w, 8, vivid->system_start_code);
    put_q(&w, 12, p->minimum_maxrgb, MAXRGB_DEN);
    put_q(&w, 12, p->average_maxrgb, MAXRGB_DEN);
    put_q(&w, 12, p->variance_maxrgb, MAXRGB_DEN);
    put_q(&w, 12, p->maximum_maxrgb, MAXRGB_DEN);

    put_bits(&w, 1, !!p->tone_mapping_mode_flag);
    if (p->tone_mapping_mode_flag) {
        int num = MPCLAMP(p->tone_mapping_param_num, 1, 2);
        put_bits(&w, 1, num - 1);
        for (int i = 0; i < num; i++)
            put_tone_mapping(&w, &p->tm_params[i]);
    }

    put_bits(&w, 1, !!p->color_saturation_mapping_flag);
    if (p->color_saturation_mapping_flag) {
        int num = MPCLAMP(p->color_saturation_num, 0, 7);
        put_bits(&w, 3, num);
        for (int i = 0; i < num; i++)
            put_q(&w, 8, p->color_saturation_gain[i], 128);
    }

    if (w.overflow)
        return 0;
    return (w.pos + 7) / 8;
}
