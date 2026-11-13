// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#pragma once
#include <array>
#include <cstdint>
#include <exiv2/exif.hpp>
#include <span>

#include "image/dng_color_profile.hpp"

namespace alcedo {
/// Read the embedded IFD0 profile. Present but malformed tables are errors, not ignored tags.
auto ReadDngColorProfile(const Exiv2::ExifData& exif) -> DngColorProfilePtr;

/**
 * @brief Build a validated hue/saturation/value table from DNG table data.
 *
 * @p data holds the entries in DNG order (value, hue, saturation; saturation fastest). DNG permits
 * omitting the zero-saturation row of each slice: when @p data has `h * (s - 1) * v * 3` entries,
 * the function inserts that row with the hue shift and saturation scale of the first stored row
 * and value scale 1. Otherwise @p data must have `h * s * v * 3` entries and each zero-saturation
 * row must have value scale 1.
 *
 * Used by the embedded DNG import and the DCP file reader.
 *
 * @param divisions Hue, saturation, and value divisions.
 * @param encoding 0 for linear value, 1 for the sRGB value coordinate.
 * @throws std::runtime_error for invalid dimensions, encoding, entry count, or entries.
 */
auto MakeDngHueSatMap(const std::array<std::uint32_t, 3>& divisions, std::uint32_t encoding,
                      std::span<const float> data) -> DngHueSatMap;
}  // namespace alcedo
