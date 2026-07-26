//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

#include "image/image.hpp"

namespace alcedo::raw_decode {

/// Runtime availability of the controls owned by the RAW Decode panel.
///
/// The application layer derives this from the imported image and build
/// capabilities.  QML only renders the resulting values and never decides
/// whether a RAW option may be used.
struct Capabilities {
  bool                     raw_source              = false;
  bool                     available               = false;
  bool                     metadata_available      = false;
  bool                     neural_engine_available = false;
  bool                     highlights_available    = false;
  std::string              unavailable_reason      = {};
  std::vector<std::string> method_values           = {};
};

inline auto IsRawImageType(const ImageType image_type) -> bool {
  switch (image_type) {
    case ImageType::ARW:
    case ImageType::CR2:
    case ImageType::CR3:
    case ImageType::NEF:
    case ImageType::DNG:
      return true;
    case ImageType::DEFAULT:
    case ImageType::JPEG:
    case ImageType::PNG:
    case ImageType::TIFF:
      return false;
  }
  return false;
}

inline auto IsRawImagePath(const std::filesystem::path& image_path) -> bool {
  auto extension = image_path.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](const unsigned char value) { return static_cast<char>(std::tolower(value)); });
  return extension == ".raw" || extension == ".arw" || extension == ".cr2" || extension == ".cr3" ||
         extension == ".nef" || extension == ".dng" || extension == ".raf" || extension == ".3fr" ||
         extension == ".rw2" || extension == ".fff";
}

inline auto FromImageMetadata(const ImageType image_type, const bool metadata_available)
    -> Capabilities {
  Capabilities result;
  result.raw_source         = IsRawImageType(image_type);
  result.metadata_available = metadata_available;
#if defined(HAVE_CUDA) || defined(HAVE_METAL) || defined(HAVE_OPENCL)
  result.neural_engine_available = result.raw_source;
#endif

  if (!result.raw_source) {
    result.unavailable_reason = "RAW Decode applies only to RAW images.";
    return result;
  }
  if (!metadata_available) {
    result.unavailable_reason = "RAW metadata is unavailable; decoder defaults are active.";
  }

  result.available            = true;
  result.highlights_available = true;
  result.method_values        = {"default", "legacy"};
  if (result.neural_engine_available) {
    result.method_values.push_back("neural_engine");
  }
  return result;
}

inline auto FromImageMetadata(const ImageType image_type, const std::filesystem::path& image_path,
                              const bool metadata_available) -> Capabilities {
  if (IsRawImageType(image_type)) {
    return FromImageMetadata(image_type, metadata_available);
  }
  if (IsRawImagePath(image_path)) {
    return FromImageMetadata(ImageType::DNG, metadata_available);
  }
  return FromImageMetadata(image_type, metadata_available);
}

}  // namespace alcedo::raw_decode
