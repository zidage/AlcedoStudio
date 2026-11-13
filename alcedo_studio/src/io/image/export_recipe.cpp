//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "io/image/export_recipe.hpp"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace alcedo {
namespace {

auto UnitsPerInch(ExportPhysicalUnit unit) -> double {
  switch (unit) {
    case ExportPhysicalUnit::INCHES:
      return 1.0;
    case ExportPhysicalUnit::CENTIMETERS:
      return 2.54;
    case ExportPhysicalUnit::MILLIMETERS:
      return 25.4;
  }
  return 1.0;
}

auto ScaleIntoBox(ExportPixelSize source, double max_width, double max_height, bool allow_upscale)
    -> ExportResolutionResult {
  if (source.width_ <= 0 || source.height_ <= 0) {
    return {.message_ = "Source pixel dimensions must be positive"};
  }
  if ((!std::isfinite(max_width) || max_width <= 0.0) &&
      (!std::isfinite(max_height) || max_height <= 0.0)) {
    return {.message_ = "At least one output dimension must be positive"};
  }

  double scale = std::numeric_limits<double>::infinity();
  if (std::isfinite(max_width) && max_width > 0.0) {
    scale = std::min(scale, max_width / static_cast<double>(source.width_));
  }
  if (std::isfinite(max_height) && max_height > 0.0) {
    scale = std::min(scale, max_height / static_cast<double>(source.height_));
  }
  if (!allow_upscale) {
    scale = std::min(scale, 1.0);
  }
  if (!std::isfinite(scale) || scale <= 0.0) {
    return {.message_ = "The output scale is invalid"};
  }

  const auto width = std::clamp<long long>(std::llround(static_cast<double>(source.width_) * scale),
                                           1, std::numeric_limits<int>::max());
  const auto height =
      std::clamp<long long>(std::llround(static_cast<double>(source.height_) * scale), 1,
                            std::numeric_limits<int>::max());
  return {.success_ = true, .pixels_ = {static_cast<int>(width), static_cast<int>(height)}};
}

auto ApplyMaximumEdgeLimit(ExportResolutionResult result, int maximum_edge_pixels)
    -> ExportResolutionResult {
  if (!result.success_ || maximum_edge_pixels <= 0 ||
      std::max(result.pixels_.width_, result.pixels_.height_) <= maximum_edge_pixels) {
    return result;
  }

  const ExportPixelSize original_pixels = result.pixels_;
  auto limited = ScaleIntoBox(result.pixels_, maximum_edge_pixels, maximum_edge_pixels, false);
  if (!limited.success_) return limited;

  limited.dpi_ = result.dpi_;
  if (limited.dpi_ > 0.0) {
    limited.dpi_ *=
        static_cast<double>(limited.pixels_.width_) / static_cast<double>(original_pixels.width_);
  }
  return limited;
}

auto TwoDigits(int value) -> std::wstring {
  std::wostringstream stream;
  stream << std::setfill(L'0') << std::setw(2) << value;
  return stream.str();
}

auto ParseCaptureDate(const std::wstring& value, int& year, int& month, int& day, int& hour,
                      int& minute, int& second) -> bool {
  if (value.size() < 10) return false;
  try {
    year   = std::stoi(value.substr(0, 4));
    month  = std::stoi(value.substr(5, 2));
    day    = std::stoi(value.substr(8, 2));
    hour   = value.size() >= 13 ? std::stoi(value.substr(11, 2)) : 0;
    minute = value.size() >= 16 ? std::stoi(value.substr(14, 2)) : 0;
    second = value.size() >= 19 ? std::stoi(value.substr(17, 2)) : 0;
  } catch (...) {
    return false;
  }
  return year > 0 && month >= 1 && month <= 12 && day >= 1 && day <= 31 && hour >= 0 &&
         hour <= 23 && minute >= 0 && minute <= 59 && second >= 0 && second <= 60;
}

auto FormatCaptureDate(const std::wstring& value, std::wstring format) -> std::wstring {
  int year   = 0;
  int month  = 0;
  int day    = 0;
  int hour   = 0;
  int minute = 0;
  int second = 0;
  if (!ParseCaptureDate(value, year, month, day, hour, minute, second)) return {};
  if (format.empty()) format = L"yyyyMMdd";

  const std::pair<std::wstring, std::wstring> replacements[] = {
      {L"yyyy", std::to_wstring(year)}, {L"MM", TwoDigits(month)},  {L"dd", TwoDigits(day)},
      {L"HH", TwoDigits(hour)},         {L"mm", TwoDigits(minute)}, {L"ss", TwoDigits(second)},
  };
  for (const auto& [token, replacement] : replacements) {
    size_t position = 0;
    while ((position = format.find(token, position)) != std::wstring::npos) {
      format.replace(position, token.size(), replacement);
      position += replacement.size();
    }
  }
  return format;
}

auto FormatNumber(double value, int precision, const wchar_t* suffix = L"") -> std::wstring {
  if (!std::isfinite(value) || value <= 0.0) return {};
  std::wostringstream stream;
  stream << std::fixed << std::setprecision(precision) << value << suffix;
  return stream.str();
}

auto PartText(const ExportFileNamePart& part, const ExportFileNameContext& context)
    -> std::wstring {
  switch (part.field_) {
    case ExportFileNameField::LITERAL:
      return part.literal_;
    case ExportFileNameField::SOURCE_STEM:
      return context.source_stem_;
    case ExportFileNameField::CAPTURE_DATE:
      return FormatCaptureDate(context.capture_date_time_, part.format_);
    case ExportFileNameField::CAMERA_MAKE:
      return context.camera_make_;
    case ExportFileNameField::CAMERA_MODEL:
      return context.camera_model_;
    case ExportFileNameField::LENS_MODEL:
      return context.lens_model_;
    case ExportFileNameField::ISO:
      return context.iso_ == 0 ? std::wstring{} : std::to_wstring(context.iso_);
    case ExportFileNameField::APERTURE:
      return FormatNumber(context.aperture_, 1);
    case ExportFileNameField::SHUTTER_SPEED:
      if (context.shutter_numerator_ <= 0 || context.shutter_denominator_ <= 0) return {};
      return std::to_wstring(context.shutter_numerator_) + L"-" +
             std::to_wstring(context.shutter_denominator_);
    case ExportFileNameField::FOCAL_LENGTH:
      return FormatNumber(context.focal_length_mm_, 1, L"mm");
    case ExportFileNameField::RATING:
      return std::to_wstring(std::clamp(context.rating_, 0, 5));
    case ExportFileNameField::SEQUENCE: {
      std::wostringstream stream;
      stream << std::setfill(L'0') << std::setw(std::max(0, part.number_width_))
             << context.sequence_;
      return stream.str();
    }
  }
  return {};
}

auto IsReservedWindowsStem(std::wstring stem) -> bool {
  std::transform(stem.begin(), stem.end(), stem.begin(),
                 [](wchar_t value) { return std::towupper(value); });
  static const std::unordered_set<std::wstring> reserved = {
      L"CON",  L"PRN",  L"AUX",  L"NUL",  L"COM1", L"COM2", L"COM3", L"COM4",
      L"COM5", L"COM6", L"COM7", L"COM8", L"COM9", L"LPT1", L"LPT2", L"LPT3",
      L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9"};
  return reserved.contains(stem);
}

auto SanitizeFileStem(std::wstring value) -> std::wstring {
  for (wchar_t& character : value) {
    if (character < 32 || character == L'<' || character == L'>' || character == L':' ||
        character == L'"' || character == L'/' || character == L'\\' || character == L'|' ||
        character == L'?' || character == L'*') {
      character = L'_';
    }
  }
  while (!value.empty() && (value.back() == L' ' || value.back() == L'.')) value.pop_back();
  while (!value.empty() && value.front() == L' ') value.erase(value.begin());
  if (IsReservedWindowsStem(value)) value += L"_";
  constexpr size_t kMaximumStemLength = 200;
  if (value.size() > kMaximumStemLength) value.resize(kMaximumStemLength);
  return value;
}

void AppendLiteralPart(ExportFileNameTemplate& name_template, std::wstring text) {
  if (text.empty()) return;
  if (!name_template.parts_.empty() &&
      name_template.parts_.back().field_ == ExportFileNameField::LITERAL) {
    name_template.parts_.back().literal_ += text;
    return;
  }
  name_template.parts_.push_back(
      ExportFileNamePart{.field_ = ExportFileNameField::LITERAL, .literal_ = std::move(text)});
}

auto ParsePatternToken(std::wstring token) -> std::optional<ExportFileNamePart> {
  std::wstring argument;
  if (const auto separator = token.find(L':'); separator != std::wstring::npos) {
    argument = token.substr(separator + 1);
    token.resize(separator);
  }
  std::transform(token.begin(), token.end(), token.begin(),
                 [](wchar_t value) { return std::towlower(value); });

  if (token == L"source" && argument.empty()) {
    return ExportFileNamePart{.field_ = ExportFileNameField::SOURCE_STEM};
  }
  if (token == L"date") {
    return ExportFileNamePart{.field_  = ExportFileNameField::CAPTURE_DATE,
                              .format_ = std::move(argument)};
  }
  if (token == L"cameramake" && argument.empty()) {
    return ExportFileNamePart{.field_ = ExportFileNameField::CAMERA_MAKE};
  }
  if (token == L"cameramodel" && argument.empty()) {
    return ExportFileNamePart{.field_ = ExportFileNameField::CAMERA_MODEL};
  }
  if (token == L"lens" && argument.empty()) {
    return ExportFileNamePart{.field_ = ExportFileNameField::LENS_MODEL};
  }
  if (token == L"iso" && argument.empty()) {
    return ExportFileNamePart{.field_ = ExportFileNameField::ISO};
  }
  if (token == L"aperture" && argument.empty()) {
    return ExportFileNamePart{.field_ = ExportFileNameField::APERTURE};
  }
  if (token == L"shutter" && argument.empty()) {
    return ExportFileNamePart{.field_ = ExportFileNameField::SHUTTER_SPEED};
  }
  if (token == L"focal" && argument.empty()) {
    return ExportFileNamePart{.field_ = ExportFileNameField::FOCAL_LENGTH};
  }
  if (token == L"rating" && argument.empty()) {
    return ExportFileNamePart{.field_ = ExportFileNameField::RATING};
  }
  if (token == L"sequence") {
    if (!argument.empty() && std::any_of(argument.begin(), argument.end(),
                                         [](wchar_t value) { return value != L'0'; })) {
      return std::nullopt;
    }
    return ExportFileNamePart{.field_        = ExportFileNameField::SEQUENCE,
                              .number_width_ = static_cast<int>(argument.size())};
  }
  return std::nullopt;
}

}  // namespace

auto ExportRecipe::FromLegacyOptions(const ExportFormatOptions& options) -> ExportRecipe {
  ExportRecipe recipe;
  recipe.codec_     = options;
  recipe.collision_ = ExportCollisionPolicy::REPLACE;
  if (options.resize_enabled_ && options.max_length_side_ > 0) {
    recipe.resize_.mode_             = ExportResizeMode::LONG_EDGE_PIXELS;
    recipe.resize_.long_edge_pixels_ = options.max_length_side_;
  }
  return recipe;
}

auto ExportRecipeHasResolvedOutputColor(const ExportRecipe& recipe) -> bool {
  return recipe.output_color_.has_value() &&
         std::isfinite(recipe.output_color_->peak_luminance) &&
         recipe.output_color_->peak_luminance > 0.0f;
}

void RequireResolvedExportOutputColor(const ExportRecipe& recipe) {
  if (!ExportRecipeHasResolvedOutputColor(recipe)) {
    throw std::runtime_error(
        "ExportRecipe: encoding space, EOTF, and peak luminance must be resolved before scheduling");
  }
}

auto ResolveExportResolution(const ExportResizeSpec& spec, ExportPixelSize source)
    -> ExportResolutionResult {
  if (source.width_ <= 0 || source.height_ <= 0) {
    return {.message_ = "Source pixel dimensions must be positive"};
  }
  ExportResolutionResult result;
  switch (spec.mode_) {
    case ExportResizeMode::ORIGINAL_PIXELS:
      result = {.success_ = true, .pixels_ = source, .dpi_ = std::max(0.0, spec.dpi_)};
      break;
    case ExportResizeMode::LONG_EDGE_PIXELS: {
      result =
          ScaleIntoBox(source, spec.long_edge_pixels_, spec.long_edge_pixels_, spec.allow_upscale_);
      result.dpi_ = std::max(0.0, spec.dpi_);
      break;
    }
    case ExportResizeMode::BOUNDING_BOX_PIXELS: {
      result = ScaleIntoBox(source, spec.width_pixels_, spec.height_pixels_, spec.allow_upscale_);
      result.dpi_ = std::max(0.0, spec.dpi_);
      break;
    }
    case ExportResizeMode::PHYSICAL_SIZE: {
      if (!std::isfinite(spec.dpi_) || spec.dpi_ <= 0.0) {
        return {.message_ = "Physical-size export requires a positive DPI"};
      }
      const double units_per_inch = UnitsPerInch(spec.physical_unit_);
      const double width_pixels =
          spec.physical_width_ > 0.0 ? spec.physical_width_ / units_per_inch * spec.dpi_ : 0.0;
      const double height_pixels =
          spec.physical_height_ > 0.0 ? spec.physical_height_ / units_per_inch * spec.dpi_ : 0.0;
      result      = ScaleIntoBox(source, width_pixels, height_pixels, spec.allow_upscale_);
      result.dpi_ = spec.dpi_;
      break;
    }
    default:
      return {.message_ = "Unknown export resize mode"};
  }
  return ApplyMaximumEdgeLimit(result, spec.maximum_edge_pixels_);
}

auto IsSupportedExportOutputFormat(ImageFormatType format) -> bool {
  switch (format) {
    case ImageFormatType::JPEG:
    case ImageFormatType::PNG:
    case ImageFormatType::TIFF:
    case ImageFormatType::EXR:
      return true;
    case ImageFormatType::WEBP:
    case ImageFormatType::BMP:
      // Deprecated export targets — do not emit these formats.
      return false;
    default:
      return false;
  }
}

auto ExportFileExtension(ImageFormatType format) -> std::wstring {
  if (!IsSupportedExportOutputFormat(format)) {
    return {};
  }
  switch (format) {
    case ImageFormatType::JPEG:
      return L".jpg";
    case ImageFormatType::PNG:
      return L".png";
    case ImageFormatType::TIFF:
      return L".tif";
    case ImageFormatType::EXR:
      return L".exr";
    default:
      return {};
  }
}

auto ResolveExportFileName(const ExportFileNameTemplate& name_template,
                           const ExportFileNameContext& context, ImageFormatType format)
    -> ExportFileNameResult {
  const std::wstring extension = ExportFileExtension(format);
  if (extension.empty()) return {.message_ = "The export format has no output extension"};

  std::wstring stem;
  for (const auto& part : name_template.parts_) stem += PartText(part, context);
  stem = SanitizeFileStem(std::move(stem));
  if (stem.empty()) stem = SanitizeFileStem(name_template.fallback_stem_);
  if (stem.empty()) return {.message_ = "The file-name template produced an empty name"};

  return {.success_ = true, .file_name_ = std::filesystem::path(stem + extension)};
}

auto ParseExportFileNamePattern(const std::wstring& pattern, std::wstring fallback_stem)
    -> ExportFileNameTemplateParseResult {
  ExportFileNameTemplate name_template;
  name_template.parts_.clear();
  name_template.fallback_stem_ = std::move(fallback_stem);

  std::wstring literal;
  for (size_t index = 0; index < pattern.size();) {
    if (pattern[index] == L'{' && index + 1 < pattern.size() && pattern[index + 1] == L'{') {
      literal.push_back(L'{');
      index += 2;
      continue;
    }
    if (pattern[index] == L'}' && index + 1 < pattern.size() && pattern[index + 1] == L'}') {
      literal.push_back(L'}');
      index += 2;
      continue;
    }
    if (pattern[index] != L'{') {
      if (pattern[index] == L'}') {
        return {.message_ = "The file-name pattern has an unmatched closing brace"};
      }
      literal.push_back(pattern[index++]);
      continue;
    }

    AppendLiteralPart(name_template, std::move(literal));
    literal.clear();
    const size_t closing = pattern.find(L'}', index + 1);
    if (closing == std::wstring::npos) {
      return {.message_ = "The file-name pattern has an unmatched opening brace"};
    }
    const std::wstring token_text = pattern.substr(index + 1, closing - index - 1);
    const auto         part       = ParsePatternToken(token_text);
    if (!part.has_value()) {
      return {.message_ = "The file-name pattern contains an unknown field"};
    }
    name_template.parts_.push_back(*part);
    index = closing + 1;
  }
  AppendLiteralPart(name_template, std::move(literal));

  if (name_template.parts_.empty()) {
    return {.message_ = "The file-name pattern is empty"};
  }
  return {.success_ = true, .name_template_ = std::move(name_template)};
}

}  // namespace alcedo
