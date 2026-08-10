//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <filesystem>
#include <optional>
#include <vector>

#include "image/metadata.hpp"

namespace alcedo {

struct JpegExifApp1Options {
  std::optional<ExifDisplayMetaData> metadata_;
  int                                width_            = 0;
  int                                height_           = 0;
  bool                               include_software_ = true;
};

/// Build a JPEG APP1 Exif payload (`Exif\0\0` + TIFF IFDs) without Exiv2.
/// Always returns a non-empty payload when `include_software_` is true.
auto BuildJpegExifApp1Payload(const JpegExifApp1Options& options) -> std::vector<uint8_t>;

/// Replace or insert the APP1 Exif segment in an existing JPEG file.
auto ReplaceJpegExifApp1Segment(const std::filesystem::path& path,
                                const std::vector<uint8_t>&  exif_payload) -> bool;

/// Build and write APP1 Exif for an exported JPEG (SDR or Ultra HDR).
auto ApplyJpegExifApp1Metadata(const std::filesystem::path& path,
                               const JpegExifApp1Options&   options) -> bool;

}  // namespace alcedo
