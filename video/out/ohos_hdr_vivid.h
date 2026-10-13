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
#include <stddef.h>
#include <stdint.h>

#include <libavutil/hdr_dynamic_vivid_metadata.h>

struct mp_image;
struct pl_color_map_params;
struct pl_color_space;
struct pl_hdr_metadata;
struct pl_tone_map_params;

// Upper bound of a serialized HDR Vivid payload: one window carrying every
// optional field takes 476 bits.
#define OHOS_VIVID_MAX_SIZE 64

// The dynamic HDR metadata a frame carries, by the format it came in.
enum ohos_dynamic_hdr {
    OHOS_DYNAMIC_HDR_NONE = 0,
    OHOS_DYNAMIC_HDR_DOVI,      // Dolby Vision RPU, levels 1 and 3
    OHOS_DYNAMIC_HDR_VIVID,     // HDR Vivid (CUVA, T/UWA 005.1)
    OHOS_DYNAMIC_HDR_PLUS,      // HDR10+ (SMPTE ST 2094-40)
};

// Per-picture maxRGB statistics in the PQ domain (0..1), the quantities HDR
// Vivid metadata describes a picture with. var is the spread of the maxRGB
// distribution in the same unit as T/UWA 005.1's variance_maxrgb.
struct ohos_vivid_stats {
    float min, avg, var, max;
};

// Finds the dynamic HDR metadata a frame carries and derives its statistics.
// Dolby Vision wins over the others, matching libplacebo's own preference.
// For native HDR Vivid, *native points at the frame's own metadata, valid for
// the frame's lifetime.
enum ohos_dynamic_hdr ohos_vivid_probe(const struct mp_image *mpi,
                                       struct ohos_vivid_stats *stats,
                                       const AVDynamicHDRVivid **native);

// Describes the Dolby Vision RPU a frame carries, for the log: its metadata
// levels in ascending order, repeated ones with their count ("2x3"), and the
// raw level 3 offsets. Empty for a frame without one. The text is cut to fit
// the size bytes of buf, one at least.
void ohos_dovi_describe(const struct mp_image *mpi, char *buf, size_t size);

// The dynamic metadata of the stream on screen. Frames that repeat a scene may
// not carry it again, so the last metadata stays until the stream changes.
struct ohos_vivid_tracker {
    enum ohos_dynamic_hdr dynamic;
    struct ohos_vivid_stats stats;
    // The stream's own HDR Vivid metadata, serialized; empty for other formats.
    uint8_t native[OHOS_VIVID_MAX_SIZE];
    size_t native_size;
    // The newest frame taken in, and whether a seek or new stream followed it.
    uint64_t frame_id;
    bool reset;
};

// A seek or a new stream. The metadata held is dropped at the first frame
// newer than any taken in so far; redrawing the frame on screen keeps it.
void ohos_vivid_tracker_reset(struct ohos_vivid_tracker *tracker);

// Takes in the frame about to be shown. frame_id is mpv's unique frame ID,
// which increases with every new frame and stays the same on redraws.
void ohos_vivid_tracker_update(struct ohos_vivid_tracker *tracker,
                               const struct mp_image *mpi, uint64_t frame_id);

// The tone mapping libplacebo renders src into dst with, set up as
// pl_shader_color_map_ex() does it; a no-op where it maps nothing. The one
// input missing is the peak libplacebo may measure on the GPU. params may be
// NULL for its defaults.
void ohos_vivid_tone_map_params(struct pl_tone_map_params *out,
                                const struct pl_color_map_params *params,
                                const struct pl_color_space *src,
                                const struct pl_color_space *dst);

// Statistics of a picture known only by its static HDR10 metadata (in nits).
void ohos_vivid_stats_from_static(struct ohos_vivid_stats *stats,
                                  const struct pl_hdr_metadata *hdr);

// Maps statistics through the tone curve libplacebo renders the picture with,
// so that they describe the output instead of the source.
void ohos_vivid_tone_map(struct ohos_vivid_stats *stats,
                         const struct pl_tone_map_params *tone);

// Builds HDR Vivid metadata that describes a picture through its statistics
// alone: no tone mapping or saturation parameters, so the system derives its
// display adaptation curve from them.
void ohos_vivid_from_stats(AVDynamicHDRVivid *out,
                           const struct ohos_vivid_stats *stats);

// Serializes HDR Vivid metadata into the form OH_HDR_DYNAMIC_METADATA takes.
// Returns the payload size, or 0 if it does not fit into size bytes.
size_t ohos_vivid_write(const AVDynamicHDRVivid *vivid, uint8_t *buf,
                        size_t size);
