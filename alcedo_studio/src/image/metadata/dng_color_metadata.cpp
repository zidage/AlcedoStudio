//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "dng_color_metadata.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "camera_name_match.hpp"
#include "image/dng_camera_matrix.hpp"
#include "image/dng_color_profile_import.hpp"
#include "metadata_values.hpp"
#include "read_only_file_mapping.hpp"

namespace alcedo::metadata_detail {
namespace {
auto ParseExifNumericToken(std::string token, double& out_value) -> bool {
  token.erase(std::remove_if(token.begin(), token.end(),
                             [](unsigned char ch) {
                               return ch == '[' || ch == ']' || ch == '(' || ch == ')' || ch == ',';
                             }),
              token.end());
  if (token.empty()) {
    return false;
  }

  const size_t slash_pos = token.find('/');
  if (slash_pos != std::string::npos) {
    const std::string numerator_text   = token.substr(0, slash_pos);
    const std::string denominator_text = token.substr(slash_pos + 1);
    char*             numerator_end    = nullptr;
    char*             denominator_end  = nullptr;
    const double      numerator        = std::strtod(numerator_text.c_str(), &numerator_end);
    const double      denominator      = std::strtod(denominator_text.c_str(), &denominator_end);
    if (numerator_end == numerator_text.c_str() || denominator_end == denominator_text.c_str() ||
        !std::isfinite(numerator) || !std::isfinite(denominator) || std::abs(denominator) < 1e-12) {
      return false;
    }
    out_value = numerator / denominator;
    return std::isfinite(out_value);
  }

  char*        value_end = nullptr;
  const double value     = std::strtod(token.c_str(), &value_end);
  if (value_end == token.c_str() || !std::isfinite(value)) {
    return false;
  }
  out_value = value;
  return true;
}

auto ParseExifNumericList(const std::string& text) -> std::vector<double> {
  std::istringstream  iss(text);
  std::string         token;
  std::vector<double> values;

  while (iss >> token) {
    double parsed_value = 0.0;
    if (ParseExifNumericToken(token, parsed_value)) {
      values.push_back(parsed_value);
    }
  }

  return values;
}

auto ReadExifStringTag(const Exiv2::ExifData& exif_data, const char* key) -> std::string {
  const auto it = exif_data.findKey(Exiv2::ExifKey(key));
  if (it == exif_data.end()) {
    return {};
  }
  return TrimAscii(it->toString());
}

auto ReadExifNumericArrayTag(const Exiv2::ExifData& exif_data, const char* key, const int count,
                             double* values_out) -> bool {
  if (!values_out || count <= 0) {
    return false;
  }

  const auto it = exif_data.findKey(Exiv2::ExifKey(key));
  if (it == exif_data.end()) {
    return false;
  }

  const auto values = ParseExifNumericList(it->toString());
  if (static_cast<int>(values.size()) < count) {
    return false;
  }

  for (int i = 0; i < count; ++i) {
    values_out[i] = values[static_cast<size_t>(i)];
  }
  return true;
}

auto ReadExifUnsignedIntTag(const Exiv2::ExifData& exif_data, const char* key, uint32_t& out_value)
    -> bool {
  const auto it = exif_data.findKey(Exiv2::ExifKey(key));
  if (it == exif_data.end()) {
    return false;
  }

  try {
    out_value = static_cast<uint32_t>(it->toUint32());
    return true;
  } catch (...) {
    try {
      out_value = static_cast<uint32_t>(it->toInt64());
      return true;
    } catch (...) {
      return false;
    }
  }
}

auto ShouldPreferUniqueCameraModel(const std::string& current_model, const std::string& exif_model,
                                   const std::string& unique_camera_model) -> bool {
  if (unique_camera_model.empty()) {
    return false;
  }

  if (!HasMeaningfulCameraModelToken(current_model)) {
    return true;
  }

  const std::string current_compact = CompactCameraName(current_model);
  const std::string exif_compact    = CompactCameraName(exif_model);
  const std::string unique_compact  = CompactCameraName(unique_camera_model);

  if (current_compact.empty() || exif_compact.empty() || unique_compact.empty()) {
    return false;
  }

  const bool current_matches_exif = current_model == exif_model || current_compact == exif_compact;
  if (!current_matches_exif) {
    return false;
  }

  return unique_compact.find(exif_compact) != std::string::npos ||
         exif_compact.find(unique_compact) != std::string::npos;
}
}  // namespace

auto ExtractDngGeometryMetadataFromFile(const std::filesystem::path& path)
    -> std::optional<dng::Metadata> {
  const ReadOnlyFileMapping mapping(path);
  if (mapping.Bytes().empty()) {
    return std::nullopt;
  }
  return dng::ExtractMetadata(mapping.Bytes());
}

void PopulateDngColorMetadataFromExif(const Exiv2::ExifData&  exif_data,
                                      RawRuntimeColorContext& ctx) {
  const std::string exif_make  = ReadExifStringTag(exif_data, "Exif.Image.Make");
  const std::string exif_model = ReadExifStringTag(exif_data, "Exif.Image.Model");
  const std::string unique_camera_model =
      ReadExifStringTag(exif_data, "Exif.Image.UniqueCameraModel");

  if (ctx.camera_make_.empty() && !exif_make.empty()) {
    ctx.camera_make_ = exif_make;
  }
  if (ShouldPreferUniqueCameraModel(ctx.camera_model_, exif_model, unique_camera_model)) {
    ctx.camera_model_ = unique_camera_model;
  } else if (ctx.camera_model_.empty() && !exif_model.empty()) {
    ctx.camera_model_ = exif_model;
  }

  double     cm1[9]  = {};
  double     cm2[9]  = {};
  const bool has_cm1 = ReadExifNumericArrayTag(exif_data, "Exif.Image.ColorMatrix1", 9, cm1);
  const bool has_cm2 = ReadExifNumericArrayTag(exif_data, "Exif.Image.ColorMatrix2", 9, cm2);
  if (!has_cm1 && !has_cm2) {
    return;
  }

  if (!has_cm1 && has_cm2) {
    std::memcpy(cm1, cm2, sizeof(cm1));
  } else if (has_cm1 && !has_cm2) {
    std::memcpy(cm2, cm1, sizeof(cm2));
  }

  std::memcpy(ctx.color_matrix_1_, cm1, sizeof(ctx.color_matrix_1_));
  std::memcpy(ctx.color_matrix_2_, cm2, sizeof(ctx.color_matrix_2_));
  ctx.color_matrices_valid_ = true;

  ctx.dng_profile_          = ReadDngColorProfile(exif_data);

  double     fm1[9]         = {};
  double     fm2[9]         = {};
  const bool has_fm1 = ReadExifNumericArrayTag(exif_data, "Exif.Image.ForwardMatrix1", 9, fm1);
  const bool has_fm2 = ReadExifNumericArrayTag(exif_data, "Exif.Image.ForwardMatrix2", 9, fm2);
  if (has_fm1 || has_fm2) {
    if (!has_fm1 && has_fm2) {
      std::memcpy(fm1, fm2, sizeof(fm1));
    } else if (has_fm1 && !has_fm2) {
      std::memcpy(fm2, fm1, sizeof(fm2));
    }

    std::memcpy(ctx.forward_matrix_1_, fm1, sizeof(ctx.forward_matrix_1_));
    std::memcpy(ctx.forward_matrix_2_, fm2, sizeof(ctx.forward_matrix_2_));
    ctx.forward_matrices_valid_ = true;
  }

  double as_shot_neutral[3] = {};
  if (ReadExifNumericArrayTag(exif_data, "Exif.Image.AsShotNeutral", 3, as_shot_neutral)) {
    std::memcpy(ctx.as_shot_neutral_, as_shot_neutral, sizeof(ctx.as_shot_neutral_));
    ctx.as_shot_neutral_valid_ =
        std::all_of(std::begin(ctx.as_shot_neutral_), std::end(ctx.as_shot_neutral_),
                    [](const double value) { return std::isfinite(value) && value > 0.0; });
  }

  uint32_t   illuminant1 = 0;
  uint32_t   illuminant2 = 0;
  const bool has_illuminant1 =
      ReadExifUnsignedIntTag(exif_data, "Exif.Image.CalibrationIlluminant1", illuminant1);
  const bool has_illuminant2 =
      ReadExifUnsignedIntTag(exif_data, "Exif.Image.CalibrationIlluminant2", illuminant2);
  if (has_illuminant1 || has_illuminant2) {
    const double cct1 = has_illuminant1 ? CalibrationIlluminantToCct(illuminant1)
                                        : std::numeric_limits<double>::quiet_NaN();
    const double cct2 = has_illuminant2 ? CalibrationIlluminantToCct(illuminant2)
                                        : std::numeric_limits<double>::quiet_NaN();
    if (std::isfinite(cct1)) {
      ctx.color_matrix_1_cct_ = cct1;
    }
    if (std::isfinite(cct2)) {
      ctx.color_matrix_2_cct_ = cct2;
    }
    ctx.calibration_illuminants_valid_ =
        std::isfinite(ctx.color_matrix_1_cct_) && std::isfinite(ctx.color_matrix_2_cct_) &&
        ctx.color_matrix_1_cct_ > 0.0 && ctx.color_matrix_2_cct_ > 0.0;
  }
}

}  // namespace alcedo::metadata_detail
