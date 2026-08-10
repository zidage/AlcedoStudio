//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <filesystem>
#include <opencv2/core.hpp>
#include <optional>

#include "io/image/image_writer.hpp"
#include "type/type.hpp"

namespace alcedo {

class UltraHdrWriter {
 public:
  static void WriteImageToPath(const image_path_t&          src_path,
                               const std::filesystem::path& export_path, const cv::Mat& rgba32f,
                               const ExportFormatOptions&         options,
                               const ExportColorProfileConfig&    color_profile,
                               std::optional<ExifDisplayMetaData> export_metadata = std::nullopt,
                               bool include_exif_metadata = true, bool embed_icc_profile = true);
};

}  // namespace alcedo
