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

#include <libplacebo/tone_mapping.h>

// The tone curve of Dolby's display management for Dolby Vision, mapping a
// shot by the average and peak its metadata gives (levels 1 and 3, see
// mp_dovi_shot()). Without an average it is libplacebo's spline.
extern const struct pl_tone_map_function mp_dovi_tone_map;
