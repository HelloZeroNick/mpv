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

#include <stdbool.h>

struct AVDOVIMetadata;
struct AVFrame;
struct pl_hdr_metadata;

// The luminance a Dolby Vision shot is mapped by: its minimum, average and
// maximum as PQ values in 0-1.
struct mp_dovi_shot {
    float min, avg, max;
};

// Level 1 holds the CM v2.9 analysis of the shot. CM v4.0 reconstructs its
// own, RGB based one as level 1 plus the offsets of level 3, whose average
// also carries the colourist's mid tone offset (Dolby Vision Color Grading
// Best Practices Guide 4.0, metadata levels). The peak never drops below what
// level 1 measured: an RGB based peak is not below the CM v2.9 one, and a
// lower one would clip highlights the picture holds. Nor does the average
// reach black, which no picture with a peak is on average. Offsets past one
// another are put back in order. false without a level 1 peak, or before
// libavutil 59.12.100, which first exports the extension blocks.
bool mp_dovi_shot(const struct AVDOVIMetadata *dovi, struct mp_dovi_shot *shot);

// Maps what libplacebo leaves out of a Dolby Vision frame into hdr, once
// pl_map_avdovi_metadata() has mapped the rest:
// - The static metadata hdr lacks: the HDR10 light levels and mastering
//   display luminance of level 6, and the mastering display primaries of
//   level 9. What hdr already holds is kept: the stream's own HDR10 SEI, and
//   the mastering luminance libplacebo maps from the RPU itself.
// - The dynamic metadata libplacebo tone maps by, max_pq_y and avg_pq_y.
//   libplacebo maps level 1 alone: from the RPU's side data
//   (pl_map_avdovi_metadata()) and, with libdovi, from the raw RPU (mapped
//   here). Level 3 then completes whatever level 1 it took, as mp_dovi_shot()
//   describes; frames it took none from are left alone.
// Level 3 and the static metadata need libavutil 59.12.100, which first
// exports the extension blocks.
void mp_dovi_map_hdr(struct pl_hdr_metadata *hdr, const struct AVFrame *frame);
