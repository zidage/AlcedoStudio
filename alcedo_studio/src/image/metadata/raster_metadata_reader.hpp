//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <filesystem>
#include <optional>

#include "image/image.hpp"
#include "image/raster_color_description.hpp"
#include "type/supported_file_type.hpp"
#include "type/type.hpp"

/// Import-time metadata of raster files (JPEG, PNG, TIFF without camera data, OpenEXR).
namespace alcedo::metadata_detail {

auto ImportCategoryFor(RasterFileKind kind) -> ImportFileCategory;

/// Body of MetadataExtractor::ClassifyRasterFile.
auto ClassifyRasterFile(const image_path_t& image_path) -> std::optional<RasterFileKind>;

/// Body of MetadataExtractor::ExtractRasterMetadata_ToImage.
void ExtractRasterMetadataToImage(const image_path_t& image_path, RasterFileKind kind,
                                  Image& image);

}  // namespace alcedo::metadata_detail
