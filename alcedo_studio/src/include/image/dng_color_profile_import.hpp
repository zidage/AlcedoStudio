// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#pragma once
#include <exiv2/exif.hpp>

#include "image/dng_color_profile.hpp"

namespace alcedo {
/// Read the embedded IFD0 profile. Present but malformed tables are errors, not ignored tags.
auto ReadDngColorProfile(const Exiv2::ExifData& exif) -> DngColorProfilePtr;
}  // namespace alcedo
