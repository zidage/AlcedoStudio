//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <filesystem>
#include <memory>
#include <optional>

#include "image/image_buffer.hpp"
#include "image/metadata.hpp"
#include "io/image/export_color_profile_config.hpp"
#include "io/image/export_recipe.hpp"
#include "type/supported_file_type.hpp"
#include "type/type.hpp"

namespace alcedo {
class ImageWriter {
 public:
  static auto ShouldWriteUltraHdr(const ExportFormatOptions&                     options,
                                  const std::optional<ExportColorProfileConfig>& color_profile)
      -> bool;

  static void WriteImageToPath(const image_path_t&          src_path,
                               std::shared_ptr<ImageBuffer> image_data, ExportFormatOptions options,
                               std::optional<ExportColorProfileConfig> color_profile = std::nullopt,
                               std::optional<ExifDisplayMetaData> export_metadata = std::nullopt);

  /**
   * Encode one rendered image with explicit resize, metadata, ICC, and alpha policies.
   *
   * The caller owns final-file collision handling. This function throws on encode failure.
   */
  static void WriteImageToPath(const image_path_t&          src_path,
                               std::shared_ptr<ImageBuffer> image_data, const ExportRecipe& recipe,
                               std::optional<ExportColorProfileConfig> color_profile = std::nullopt,
                               std::optional<ExifDisplayMetaData> export_metadata = std::nullopt);
};
};  // namespace alcedo
