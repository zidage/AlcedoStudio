//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "type/supported_file_type.hpp"

namespace alcedo {

enum class ExportResizeMode : uint8_t {
  ORIGINAL_PIXELS,
  LONG_EDGE_PIXELS,
  BOUNDING_BOX_PIXELS,
  PHYSICAL_SIZE,
};

enum class ExportPhysicalUnit : uint8_t { INCHES, CENTIMETERS, MILLIMETERS };

struct ExportResizeSpec {
  ExportResizeMode   mode_                = ExportResizeMode::ORIGINAL_PIXELS;
  int                long_edge_pixels_    = 0;
  int                width_pixels_        = 0;
  int                height_pixels_       = 0;
  double             physical_width_      = 0.0;
  double             physical_height_     = 0.0;
  ExportPhysicalUnit physical_unit_       = ExportPhysicalUnit::INCHES;
  double             dpi_                 = 0.0;
  bool               allow_upscale_       = false;
  int                maximum_edge_pixels_ = 0;
};

struct ExportPixelSize {
  int width_  = 0;
  int height_ = 0;
};

struct ExportResolutionResult {
  bool            success_ = false;
  ExportPixelSize pixels_;
  double          dpi_ = 0.0;
  std::string     message_;
};

enum class ExportMetadataMode : uint8_t { NONE, STANDARD };

struct ExportMetadataPolicy {
  ExportMetadataMode mode_                    = ExportMetadataMode::STANDARD;
  bool               include_exif_            = true;
  bool               include_xmp_             = true;
  bool               include_iptc_            = true;
  bool               include_location_        = false;
  bool               include_device_serials_  = false;
  bool               include_editing_history_ = false;
};

enum class ExportIccPolicy : uint8_t { EMBED_OUTPUT_PROFILE, OMIT };
enum class ExportAlphaPolicy : uint8_t { PRESERVE_IF_SUPPORTED, DISCARD };
enum class ExportCollisionPolicy : uint8_t { FAIL, REPLACE };

enum class ExportFileNameField : uint8_t {
  LITERAL,
  SOURCE_STEM,
  CAPTURE_DATE,
  CAMERA_MAKE,
  CAMERA_MODEL,
  LENS_MODEL,
  ISO,
  APERTURE,
  SHUTTER_SPEED,
  FOCAL_LENGTH,
  RATING,
  SEQUENCE,
};

struct ExportFileNamePart {
  ExportFileNameField field_ = ExportFileNameField::LITERAL;
  std::wstring        literal_;
  std::wstring        format_;
  int                 number_width_ = 0;
};

struct ExportFileNameTemplate {
  std::vector<ExportFileNamePart> parts_ = {
      ExportFileNamePart{.field_ = ExportFileNameField::SOURCE_STEM}};
  std::wstring fallback_stem_ = L"image";
};

struct ExportFileNameTemplateParseResult {
  bool                   success_ = false;
  ExportFileNameTemplate name_template_;
  std::string            message_;
};

struct ExportFileNameContext {
  std::wstring source_stem_;
  std::wstring capture_date_time_;
  std::wstring camera_make_;
  std::wstring camera_model_;
  std::wstring lens_model_;
  uint64_t     iso_                 = 0;
  double       aperture_            = 0.0;
  int          shutter_numerator_   = 0;
  int          shutter_denominator_ = 0;
  double       focal_length_mm_     = 0.0;
  int          rating_              = 0;
  uint64_t     sequence_            = 1;
};

struct ExportFileNameResult {
  bool                  success_ = false;
  std::filesystem::path file_name_;
  std::string           message_;
};

struct ExportRecipe {
  ExportFormatOptions    codec_;
  ExportResizeSpec       resize_;
  ExportMetadataPolicy   metadata_;
  ExportIccPolicy        icc_       = ExportIccPolicy::EMBED_OUTPUT_PROFILE;
  ExportAlphaPolicy      alpha_     = ExportAlphaPolicy::PRESERVE_IF_SUPPORTED;
  ExportCollisionPolicy  collision_ = ExportCollisionPolicy::FAIL;
  ExportFileNameTemplate file_name_;

  /** Create a recipe that preserves the behavior of the former flat options. */
  static auto            FromLegacyOptions(const ExportFormatOptions& options) -> ExportRecipe;
};

/** Resolve output pixels and optional physical-resolution metadata. */
auto ResolveExportResolution(const ExportResizeSpec& spec, ExportPixelSize source)
    -> ExportResolutionResult;

/** Build and sanitize one file name. The encoder controls the extension. */
auto ResolveExportFileName(const ExportFileNameTemplate& name_template,
                           const ExportFileNameContext& context, ImageFormatType format)
    -> ExportFileNameResult;

/** Parse a user pattern such as "{date:yyyy MM dd}-{sequence:0000}". */
auto ParseExportFileNamePattern(const std::wstring& pattern, std::wstring fallback_stem = L"image")
    -> ExportFileNameTemplateParseResult;

/** Return the canonical extension for an export format. */
auto ExportFileExtension(ImageFormatType format) -> std::wstring;

}  // namespace alcedo
