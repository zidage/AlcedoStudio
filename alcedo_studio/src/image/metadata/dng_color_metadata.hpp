//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <exiv2/exif.hpp>
#include <filesystem>
#include <optional>

#include "decoders/dng_default_crop.hpp"
#include "decoders/processor/raw_color_context.hpp"

namespace alcedo::metadata_detail {

/// DNG geometry tags (default crop, WarpRectilinear) of the file at @p path.
/// @return std::nullopt when the file cannot be mapped.
auto ExtractDngGeometryMetadataFromFile(const std::filesystem::path& path)
    -> std::optional<dng::Metadata>;

/// Camera names, color and forward matrices, as-shot neutral, calibration illuminants and the DNG
/// color profile from the IFD0 tags of a DNG file.
void PopulateDngColorMetadataFromExif(const Exiv2::ExifData&  exif_data,
                                      RawRuntimeColorContext& ctx);

}  // namespace alcedo::metadata_detail
