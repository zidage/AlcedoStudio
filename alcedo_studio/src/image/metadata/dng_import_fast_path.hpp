//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <filesystem>

#include "image/image.hpp"
#include "type/type.hpp"

namespace alcedo::metadata_detail {

/// DNG import without LibRaw unpack: Exiv2 reads the display metadata and the DNG color tags, and
/// LibRaw open_file reads the camera and lens strings and admits the file as RAW.
/// @return false when Exiv2 or LibRaw cannot open the file; @p image is then unchanged.
auto ExtractDngMetadataToImageFast(const image_path_t& image_path, Image& image) -> bool;

}  // namespace alcedo::metadata_detail
