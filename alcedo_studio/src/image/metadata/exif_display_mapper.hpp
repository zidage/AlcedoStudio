//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <exiv2/exif.hpp>
#include <exiv2/image.hpp>

#include "image/metadata.hpp"

/// Exiv2 EXIF and XMP to ExifDisplayMetaData.
namespace alcedo::metadata_detail {

/// Camera, lens, exposure, size and capture time from the standard EXIF tags.
void GetDisplayMetadataFromExif(Exiv2::ExifData& exif_data, ExifDisplayMetaData& display_metadata);

/// Rating from `Xmp.xmp.Rating`, then `Exif.Image.Rating`.
void PopulateStandardRating(const Exiv2::Image& exif_image, ExifDisplayMetaData& display_metadata);

}  // namespace alcedo::metadata_detail
