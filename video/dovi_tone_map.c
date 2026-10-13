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

// Dolby's display management maps a Dolby Vision shot onto the target display
// by its minimum, average (the mid) and maximum, which CM v4.0 metadata give
// as level 1 plus level 3. This is the curve of US 10,600,166 B2 (Pytlarz,
// Atkins), Table 1 with its monotonic "cutoff" variant, which Dolby's later
// filings (US 11,336,895 B2, WO 2023/056267 A1) name as the curve for levels
// 1, 3 and 8. Everything is in PQ.
//
// The mid is placed first. It stays where it is (1:1) wherever the target has
// room around it for the shot's shadows and highlights, so that the picture
// keeps its brightness and only its highlights are compressed. Where the
// highlights would not fit above it, it is placed lower; where the shadows
// would not fit above the target's black, higher. Cubic Hermite segments then
// join the black, the mid and the peak, their slopes within the
// Fritsch-Carlson bounds (at most three times the secant), so that the curve
// never decreases.
//
// A level 3 mid offset moves the mid the shot is mapped by, not the picture. A
// positive one makes the shot brighter than it is, and once its mid no longer
// fits under the highlights it is placed lower, darkening the picture. A
// negative one makes it darker than it is; Dolby's target displays have a
// black of 0.005 cd/m2, and a mid under it is kept above it, which lifts the
// shadows and the black: the washed out look such a shot has on a Dolby
// Vision display. A shot whose mid stays clear of that black keeps the
// target's own black (absolute black for PQ).

#include <math.h>

#include "common/common.h"
#include "dovi_tone_map.h"

// The patent's example tuning: where the mid leaves the 1:1 line, shadows and
// highlights weigh alike, and each keeps half of its range.
#define MID_CUTOFF 0.5f
#define PRESERVATION 0.5f
// The black of Dolby's target displays, in cd/m2.
#define TARGET_BLACK 0.005f
// A segment narrower than this (in PQ) is empty.
#define MIN_SPAN 1e-6f

struct knot {
    float x, y, slope;
};

static float hermite(float x, const struct knot *a, const struct knot *b)
{
    float span = b->x - a->x;
    float t = (x - a->x) / span;
    float t2 = t * t, t3 = t2 * t;
    return (2 * t3 - 3 * t2 + 1) * a->y + (t3 - 2 * t2 + t) * span * a->slope +
           (3 * t2 - 2 * t3) * b->y + (t3 - t2) * span * b->slope;
}

// Where Table 1 places the shot's mid within the target's range [tmin, tmax].
static float target_mid(float smin, float smid, float smax,
                        float tmin, float tmax)
{
    float range = tmax - tmin;
    float loc = (smid - tmin) / range;
    float headroom = (smax - smid) / range, tailroom = (smid - smin) / range;
    float head = MPMIN(MPMAX(loc - MID_CUTOFF, 0),
                       MPMAX(PRESERVATION * headroom + loc - 1, 0));
    float tail = MPMIN(MPMAX(MID_CUTOFF - loc, 0),
                       MPMAX(PRESERVATION * tailroom - loc, 0));
    return smid + (tail - head) * range;
}

static float secant(const struct knot *a, const struct knot *b)
{
    float span = b->x - a->x;
    return span > MIN_SPAN ? (b->y - a->y) / span : INFINITY;
}

static float segment(float x, const struct knot *a, const struct knot *b)
{
    return b->x - a->x > MIN_SPAN ? hermite(x, a, b) : b->y;
}

static void dovi_map(float *lut, const struct pl_tone_map_params *params)
{
    // Without an average (level 1) there is no shot to map by.
    if (params->input_avg <= 0) {
        pl_tone_map_spline.map(lut, params);
        return;
    }

    float smin = params->input_min, smid = params->input_avg;
    float smax = params->input_max;
    float black = pl_hdr_rescale(PL_HDR_NITS, PL_HDR_PQ, TARGET_BLACK);
    float tmid = target_mid(smin, smid, smax,
                            MPMAX(params->output_min, black),
                            params->output_max);

    // The black moves with the mid, never under the target's own. The peak
    // lands on the target's (where Table 1 caps it): libplacebo maps only a
    // peak above the target, and the mid never moves down by more than that.
    struct knot lo = {smin, MPMAX(tmid - smid + smin, params->output_min)};
    struct knot mid = {smid, tmid};
    struct knot hi = {smax, params->output_max};
    // Both secants are at most 1, so Table 1's other bounds on the end
    // slopes (three times the secant, and 1) never bind.
    float lo_secant = secant(&lo, &mid), hi_secant = secant(&mid, &hi);
    lo.slope = lo_secant * lo_secant;
    hi.slope = powf(hi_secant, 4);
    mid.slope = MPMIN(MPMIN(3 * lo_secant, 3 * hi_secant), 1 - smid + tmid);

    for (int i = 0; i < params->lut_size; i++) {
        lut[i] = lut[i] < mid.x ? segment(lut[i], &lo, &mid)
                                : segment(lut[i], &mid, &hi);
    }
}

const struct pl_tone_map_function mp_dovi_tone_map = {
    .name = "dolbyvision",
    .description = "Dolby Vision display management",
    .scaling = PL_HDR_PQ,
    .map = dovi_map,
};
