//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

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

/// Import file categories (raster_image_input_plan.md, section 9.1). The extension decides which
/// files a folder scan lists; the file content decides whether import accepts the file.
enum class ImportFileCategory : uint8_t { Raw, Jpeg, Tiff, Png, OpenExr, Other };

/// One bit per category except Other, which is never imported.
using ImportCategoryMask                                                 = uint8_t;

inline constexpr std::array<ImportFileCategory, 5> kImportableCategories = {
    ImportFileCategory::Raw, ImportFileCategory::Jpeg, ImportFileCategory::Tiff,
    ImportFileCategory::Png, ImportFileCategory::OpenExr};

[[nodiscard]] constexpr auto ImportCategoryBit(ImportFileCategory category) -> ImportCategoryMask {
  return category == ImportFileCategory::Other
             ? ImportCategoryMask{0}
             : static_cast<ImportCategoryMask>(1u << static_cast<uint8_t>(category));
}

inline constexpr ImportCategoryMask kAllImportCategories = 0x1F;

[[nodiscard]] constexpr auto        operator|(ImportFileCategory lhs, ImportFileCategory rhs)
    -> ImportCategoryMask {
  return static_cast<ImportCategoryMask>(ImportCategoryBit(lhs) | ImportCategoryBit(rhs));
}

[[nodiscard]] constexpr auto operator|(ImportCategoryMask lhs, ImportFileCategory rhs)
    -> ImportCategoryMask {
  return static_cast<ImportCategoryMask>(lhs | ImportCategoryBit(rhs));
}

[[nodiscard]] constexpr auto CategoryAllowed(ImportCategoryMask mask, ImportFileCategory category)
    -> bool {
  return (mask & ImportCategoryBit(category)) != 0;
}

/// RAW extensions, also listed in docs/supported_raw_formats.md.
inline constexpr std::array<std::string_view, 26> kRawExtensions = {
    ".3fr", ".arw", ".cr2", ".cr3", ".crw", ".dcr", ".dng", ".erf", ".fff",
    ".iiq", ".kdc", ".mef", ".mos", ".mrw", ".nef", ".nrw", ".orf", ".pef",
    ".raf", ".raw", ".rw2", ".rwl", ".sr2", ".srf", ".srw", ".x3f"};

/// Category of the extension @p ext, with its leading dot. Matching ignores ASCII case.
[[nodiscard]] inline auto CategoryForExtension(std::string_view ext) -> ImportFileCategory {
  if (ext.size() > 8) {
    return ImportFileCategory::Other;
  }
  std::string lower(ext);
  for (auto& c : lower) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  if (lower == ".jpg" || lower == ".jpeg" || lower == ".jpe" || lower == ".jfif") {
    return ImportFileCategory::Jpeg;
  }
  if (lower == ".tif" || lower == ".tiff") return ImportFileCategory::Tiff;
  if (lower == ".png") return ImportFileCategory::Png;
  if (lower == ".exr") return ImportFileCategory::OpenExr;
  for (const auto raw : kRawExtensions) {
    if (lower == raw) return ImportFileCategory::Raw;
  }
  return ImportFileCategory::Other;
}

/// Category of the extension of @p path. A non-ASCII extension is Other.
[[nodiscard]] inline auto CategoryForPath(const fs::path& path) -> ImportFileCategory {
  const auto  wide = path.extension().wstring();
  std::string ext;
  ext.reserve(wide.size());
  for (const wchar_t c : wide) {
    if (c > 0x7F) return ImportFileCategory::Other;
    ext.push_back(static_cast<char>(c));
  }
  return CategoryForExtension(ext);
}

/// True for a regular file with a RAW extension.
inline bool is_supported_file(const fs::path& path) {
  if (!fs::is_regular_file(path)) return false;
  return CategoryForPath(path) == ImportFileCategory::Raw;
}
};  // namespace alcedo
