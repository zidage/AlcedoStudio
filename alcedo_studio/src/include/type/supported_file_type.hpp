//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <filesystem>
#include <string>
#include <unordered_set>

namespace fs = std::filesystem;

namespace alcedo {
// Image format types (should used in import and export modules)
enum class ImageFormatType : uint8_t {
  JPEG,
  PNG,
  TIFF,
  WEBP,  ///< Deprecated for export; kept for enum stability / import sniffing.
  DNG,
  ARW,
  RAW,
  CR2,
  CR3,
  NEF,
  BMP,  ///< Deprecated for export; kept for enum stability / import sniffing.
  RAF,
  _3FR,
  RW2,
  EXR,
  FFF
};

struct ExportFormatOptions {
  enum class TIFF_COMPRESS : uint8_t { NONE = 1, LZW = 5, ZIP = 8 };

  enum class BIT_DEPTH : uint8_t { BIT_8 = 8, BIT_16 = 16, BIT_32 = 32 };

  enum class HDR_EXPORT_MODE : uint8_t {
    ULTRA_HDR,
    EMBEDDED_PROFILE_ONLY,
  };

  std::filesystem::path export_path_;

  ImageFormatType       format_            = ImageFormatType::JPEG;
  bool                  resize_enabled_    = false;
  int                   max_length_side_   = 0;  // 0 means no resizing

  int                   quality_           = 95;                   // For JPEG
  BIT_DEPTH             bit_depth_         = BIT_DEPTH::BIT_16;    // For TIFF/PNG
  int                   compression_level_ = 5;                    // For PNG
  TIFF_COMPRESS         tiff_compress_     = TIFF_COMPRESS::NONE;  // For TIFF
  HDR_EXPORT_MODE       hdr_export_mode_   = HDR_EXPORT_MODE::ULTRA_HDR;
  int                   ultra_hdr_quality_ = 95;  // Gain map quality for Ultra HDR
  bool                  ultra_hdr_dither_enabled_ = true;
};

static const std::unordered_set<std::wstring> supported_extensions = {
    L".raw", L".cr2", L".nef", L".dng", L".arw", L".cr3", L".raf", L".3fr", L".rw2",
    L".RAW", L".CR2", L".NEF", L".DNG", L".ARW", L".CR3", L".RAF", L".3FR", L".RW2", L".fff", L".FFF"};

inline bool is_supported_file(const fs::path& path) {
  if (!fs::is_regular_file(path)) return false;

  std::wstring ext = path.extension().wstring();
  return supported_extensions.count(ext) > 0;
}
};  // namespace alcedo
