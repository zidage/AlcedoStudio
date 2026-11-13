//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <exiv2/image.hpp>
#include <filesystem>

#include "type/type.hpp"

namespace alcedo::metadata_detail {

/// Ultra HDR, HDR gain map or Rec.2100 markers in the XMP of @p exif_image (when not null), the
/// OpenImageIO header, or the first 4 MiB of the file.
auto DetectHdrMetadata(const image_path_t& image_path, const Exiv2::Image* exif_image = nullptr)
    -> bool;

}  // namespace alcedo::metadata_detail
