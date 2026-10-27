//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/mask/mask_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>

#ifdef ALCEDO_ENABLE_BRUSH_MASK
#include "edit/mask/brush_stroke.hpp"
#endif

namespace alcedo {
namespace {

constexpr float kMinLinearGradientNormalLength = 1.0e-6f;
/// Float rounding allowance of `x + w <= 1` and `y + h <= 1` for AI Mask reference bounds.
constexpr float       kAiReferenceBoundsTolerance    = 1.0e-6f;
constexpr std::size_t kHex128Digits                  = 32;

[[noreturn]] void Fail(std::string_view message) {
  throw std::runtime_error(std::string{message});
}

void RequireFinite(float value, std::string_view name) {
  if (!std::isfinite(value)) {
    Fail(std::string{name} + " must be finite");
  }
}

void RequireNonNegative(float value, std::string_view name) {
  RequireFinite(value, name);
  if (value < 0.0f) {
    Fail(std::string{name} + " must be nonnegative");
  }
}

void RequireObject(const nlohmann::json& json, std::string_view name) {
  if (!json.is_object()) {
    Fail(std::string{name} + " must be an object");
  }
}

void RequireNumber(const nlohmann::json& json, const char* key, std::string_view owner) {
  if (!json.contains(key) || !json[key].is_number()) {
    Fail(std::string{owner} + " is missing number " + key);
  }
}

auto ReadRequiredUint32(const nlohmann::json& json, const char* key, std::string_view owner)
    -> std::uint32_t {
  if (!json.contains(key) || !json[key].is_number() || json[key].is_number_float()) {
    Fail(std::string{owner} + " is missing unsigned integer " + key);
  }
  if (json[key].is_number_integer() && json[key].get<std::int64_t>() < 0) {
    Fail(std::string{owner} + " field " + key + " must fit in uint32");
  }
  const auto value = json[key].get<std::uint64_t>();
  if (value > std::numeric_limits<std::uint32_t>::max()) {
    Fail(std::string{owner} + " field " + key + " must fit in uint32");
  }
  return static_cast<std::uint32_t>(value);
}

#ifdef ALCEDO_ENABLE_BRUSH_MASK
auto TranslationToJson(Vector2 translation) -> nlohmann::json {
  return nlohmann::json::array({translation.x, translation.y});
}

auto TranslationFromJson(const nlohmann::json& json, std::string_view owner) -> Vector2 {
  if (!json.is_array() || json.size() < 2) {
    Fail(std::string{owner} + " placement_translation must be an array of two numbers");
  }
  Vector2 translation;
  translation.x = json[0].get<float>();
  translation.y = json[1].get<float>();
  RequireFinite(translation.x, "placement_translation.x");
  RequireFinite(translation.y, "placement_translation.y");
  return translation;
}
#endif

auto ReadRequiredFloat(const nlohmann::json& json, const char* key, std::string_view owner)
    -> float {
  RequireNumber(json, key, owner);
  const auto value = json[key].get<float>();
  RequireFinite(value, key);
  return value;
}

#ifdef ALCEDO_ENABLE_BRUSH_MASK
void ValidateBrush(const BrushMaskSource& brush) {
  RequireNonNegative(brush.feather_radius, "feather_radius");
  RequireFinite(brush.placement_translation.x, "placement_translation.x");
  RequireFinite(brush.placement_translation.y, "placement_translation.y");
  if (brush.source_format_version != kBrushSourceFormatVersion) {
    Fail("brush source_format_version must be 1");
  }
  if (brush.raster_algorithm_version != kBrushRasterAlgorithmVersion) {
    Fail("brush raster_algorithm_version must be 1");
  }
  ValidateBrushStrokeList(brush.strokes);
  RequireFinite(brush.descriptor.reference_bounds.x, "reference_bounds.x");
  RequireFinite(brush.descriptor.reference_bounds.y, "reference_bounds.y");
  RequireFinite(brush.descriptor.reference_bounds.w, "reference_bounds.w");
  RequireFinite(brush.descriptor.reference_bounds.h, "reference_bounds.h");
  if (!brush.asset_key.has_value() || brush.asset_key->Empty()) {
    return;
  }
  const auto extent = brush.descriptor.extent;
  if (extent.Empty() || extent.width > kMaximumRasterMaskAxis ||
      extent.height > kMaximumRasterMaskAxis) {
    Fail("Brush raster axes must be in [1, 4096] when an asset key is present");
  }
}
#endif

void ValidateRadial(const RadialMaskSource& radial) {
  RequireFinite(radial.center_x, "center_x");
  RequireFinite(radial.center_y, "center_y");
  RequireNonNegative(radial.major_radius, "major_radius");
  RequireNonNegative(radial.minor_radius, "minor_radius");
  RequireFinite(radial.rotation, "rotation");
  RequireNonNegative(radial.inner_feather, "inner_feather");
  RequireNonNegative(radial.outer_feather, "outer_feather");
}

void ValidateLinearGradient(const LinearGradientMaskSource& gradient) {
  RequireFinite(gradient.origin_x, "origin_x");
  RequireFinite(gradient.origin_y, "origin_y");
  RequireFinite(gradient.normal_x, "normal_x");
  RequireFinite(gradient.normal_y, "normal_y");
  RequireNonNegative(gradient.transition_distance, "transition_distance");
  RequireFinite(gradient.start_value, "start_value");
  RequireFinite(gradient.end_value, "end_value");
  const auto length = std::hypot(gradient.normal_x, gradient.normal_y);
  if (!(length > kMinLinearGradientNormalLength)) {
    Fail("Linear Gradient normal must have a valid direction");
  }
}

auto IsLowercaseHex128(std::string_view text) -> bool {
  return text.size() == kHex128Digits && std::all_of(text.begin(), text.end(), [](char ch) {
           return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
         });
}

void RequireAxis(std::uint32_t value, std::string_view name) {
  if (value < 1 || value > kMaximumRasterMaskAxis) {
    Fail(std::string{name} + " must be in [1, 4096]");
  }
}

void ValidateAiSegmentation(const AiSegmentationMaskSource& ai) {
  if (!IsLowercaseHex128(ai.asset_key.Value())) {
    Fail("AI Mask asset_key must be 32 lowercase hexadecimal digits");
  }
  RequireAxis(ai.analysis_extent.width, "AI Mask analysis_width");
  RequireAxis(ai.analysis_extent.height, "AI Mask analysis_height");
  RequireAxis(ai.descriptor.extent.width, "AI Mask asset_width");
  RequireAxis(ai.descriptor.extent.height, "AI Mask asset_height");
  if (ai.descriptor.extent.width > ai.analysis_extent.width ||
      ai.descriptor.extent.height > ai.analysis_extent.height) {
    Fail("AI Mask asset extent must not exceed the analysis extent");
  }
  const auto& bounds = ai.descriptor.reference_bounds;
  RequireFinite(bounds.x, "AI Mask reference_bounds.x");
  RequireFinite(bounds.y, "AI Mask reference_bounds.y");
  RequireFinite(bounds.w, "AI Mask reference_bounds.w");
  RequireFinite(bounds.h, "AI Mask reference_bounds.h");
  if (bounds.x < 0.0f || bounds.y < 0.0f || !(bounds.w > 0.0f) || !(bounds.h > 0.0f) ||
      bounds.x + bounds.w > 1.0f + kAiReferenceBoundsTolerance ||
      bounds.y + bounds.h > 1.0f + kAiReferenceBoundsTolerance) {
    Fail("AI Mask reference_bounds must lie inside [0, 1]");
  }
  ValidateAiMaskEdgeAdjustment(ai.edge);
  if (ai.model_set_id.empty()) {
    Fail("AI Mask model_set_id must not be empty");
  }
  if (!IsLowercaseHex128(ai.geometry_fingerprint)) {
    Fail("AI Mask geometry_fingerprint must be 32 lowercase hexadecimal digits");
  }
  ValidateAiMaskRecipe(ai.recipe);
}

#ifdef ALCEDO_ENABLE_BRUSH_MASK
auto BrushToJson(const BrushMaskSource& brush) -> nlohmann::json {
  return {{"kind", "brush"},
          {"source_format_version", brush.source_format_version},
          {"raster_algorithm_version", brush.raster_algorithm_version},
          {"placement_translation", TranslationToJson(brush.placement_translation)},
          {"feather_radius", brush.feather_radius},
          {"strokes", BrushStrokeListToJson(brush.strokes)}};
}
#endif

auto RadialToJson(const RadialMaskSource& radial) -> nlohmann::json {
  return {{"kind", "radial"},
          {"center_x", radial.center_x},
          {"center_y", radial.center_y},
          {"major_radius", radial.major_radius},
          {"minor_radius", radial.minor_radius},
          {"rotation", radial.rotation},
          {"inner_feather", radial.inner_feather},
          {"outer_feather", radial.outer_feather}};
}

auto LinearGradientToJson(const LinearGradientMaskSource& gradient) -> nlohmann::json {
  return {{"kind", "linear_gradient"},
          {"origin_x", gradient.origin_x},
          {"origin_y", gradient.origin_y},
          {"normal_x", gradient.normal_x},
          {"normal_y", gradient.normal_y},
          {"transition_distance", gradient.transition_distance},
          {"start_value", gradient.start_value},
          {"end_value", gradient.end_value}};
}

auto AiSegmentationToJson(const AiSegmentationMaskSource& ai) -> nlohmann::json {
  const auto& bounds = ai.descriptor.reference_bounds;
  return {
      {"kind", "ai_segmentation"},
      {"format_version", kAiSegmentationSourceFormatVersion},
      {"asset_key", std::string{ai.asset_key.Value()}},
      {"asset_width", ai.descriptor.extent.width},
      {"asset_height", ai.descriptor.extent.height},
      {"reference_bounds", {{"x", bounds.x}, {"y", bounds.y}, {"w", bounds.w}, {"h", bounds.h}}},
      {"analysis_width", ai.analysis_extent.width},
      {"analysis_height", ai.analysis_extent.height},
      {"feather_px", ai.edge.feather_px},
      {"expand_px", ai.edge.expand_px},
      {"model_set_id", ai.model_set_id},
      {"geometry_fingerprint", ai.geometry_fingerprint},
      {"components", AiMaskRecipeToJson(ai.recipe)}};
}

#ifdef ALCEDO_ENABLE_BRUSH_MASK
auto BrushFromJson(const nlohmann::json& json) -> BrushMaskSource {
  if (json.contains("asset_key") || json.contains("width") || json.contains("height") ||
      json.contains("reference_bounds")) {
    Fail("brush raster-only asset_key encoding is not supported");
  }
  BrushMaskSource brush;
  brush.feather_radius = ReadRequiredFloat(json, "feather_radius", "brush");
  brush.source_format_version = ReadRequiredUint32(json, "source_format_version", "brush");
  brush.raster_algorithm_version =
      ReadRequiredUint32(json, "raster_algorithm_version", "brush");
  if (brush.source_format_version != kBrushSourceFormatVersion) {
    Fail("brush source_format_version must be 1");
  }
  if (brush.raster_algorithm_version != kBrushRasterAlgorithmVersion) {
    Fail("brush raster_algorithm_version must be 1");
  }
  if (!json.contains("placement_translation")) {
    Fail("brush is missing placement_translation");
  }
  if (!json.contains("strokes")) {
    Fail("brush is missing strokes");
  }
  brush.placement_translation = TranslationFromJson(json["placement_translation"], "brush");
  brush.strokes               = BrushStrokeListFromJson(json["strokes"]);
  return brush;
}
#endif

auto RadialFromJson(const nlohmann::json& json) -> RadialMaskSource {
  RadialMaskSource radial;
  radial.center_x      = ReadRequiredFloat(json, "center_x", "radial");
  radial.center_y      = ReadRequiredFloat(json, "center_y", "radial");
  radial.major_radius  = ReadRequiredFloat(json, "major_radius", "radial");
  radial.minor_radius  = ReadRequiredFloat(json, "minor_radius", "radial");
  radial.rotation      = ReadRequiredFloat(json, "rotation", "radial");
  radial.inner_feather = ReadRequiredFloat(json, "inner_feather", "radial");
  radial.outer_feather = ReadRequiredFloat(json, "outer_feather", "radial");
  return radial;
}

auto LinearGradientFromJson(const nlohmann::json& json) -> LinearGradientMaskSource {
  LinearGradientMaskSource gradient;
  gradient.origin_x            = ReadRequiredFloat(json, "origin_x", "linear_gradient");
  gradient.origin_y            = ReadRequiredFloat(json, "origin_y", "linear_gradient");
  gradient.normal_x            = ReadRequiredFloat(json, "normal_x", "linear_gradient");
  gradient.normal_y            = ReadRequiredFloat(json, "normal_y", "linear_gradient");
  gradient.transition_distance = ReadRequiredFloat(json, "transition_distance", "linear_gradient");
  gradient.start_value         = ReadRequiredFloat(json, "start_value", "linear_gradient");
  gradient.end_value           = ReadRequiredFloat(json, "end_value", "linear_gradient");
  return gradient;
}

void RequireOnlyKeys(const nlohmann::json& json, std::string_view owner,
                     std::initializer_list<std::string_view> keys) {
  for (const auto& [key, value] : json.items()) {
    if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
      Fail(std::string{owner} + " contains unsupported field " + key);
    }
  }
}

auto ReadRequiredString(const nlohmann::json& json, const char* key, std::string_view owner)
    -> std::string {
  if (!json.contains(key) || !json[key].is_string()) {
    Fail(std::string{owner} + " is missing string " + key);
  }
  return json[key].get<std::string>();
}

auto AiSegmentationFromJson(const nlohmann::json& json) -> AiSegmentationMaskSource {
  constexpr std::string_view kOwner = "ai_segmentation";
  RequireOnlyKeys(json, kOwner,
                  {"kind", "format_version", "asset_key", "asset_width", "asset_height",
                   "reference_bounds", "analysis_width", "analysis_height", "feather_px",
                   "expand_px", "model_set_id", "geometry_fingerprint", "components"});
  if (ReadRequiredUint32(json, "format_version", kOwner) != kAiSegmentationSourceFormatVersion) {
    Fail("ai_segmentation format_version must be 1");
  }
  AiSegmentationMaskSource ai;
  ai.asset_key                = MaskAssetKey{ReadRequiredString(json, "asset_key", kOwner)};
  ai.descriptor.extent.width  = ReadRequiredUint32(json, "asset_width", kOwner);
  ai.descriptor.extent.height = ReadRequiredUint32(json, "asset_height", kOwner);
  if (!json.contains("reference_bounds")) {
    Fail("ai_segmentation is missing reference_bounds");
  }
  const auto& bounds = json["reference_bounds"];
  RequireObject(bounds, "ai_segmentation reference_bounds");
  RequireOnlyKeys(bounds, "ai_segmentation reference_bounds", {"x", "y", "w", "h"});
  ai.descriptor.reference_bounds.x = ReadRequiredFloat(bounds, "x", "reference_bounds");
  ai.descriptor.reference_bounds.y = ReadRequiredFloat(bounds, "y", "reference_bounds");
  ai.descriptor.reference_bounds.w = ReadRequiredFloat(bounds, "w", "reference_bounds");
  ai.descriptor.reference_bounds.h = ReadRequiredFloat(bounds, "h", "reference_bounds");
  ai.analysis_extent.width         = ReadRequiredUint32(json, "analysis_width", kOwner);
  ai.analysis_extent.height        = ReadRequiredUint32(json, "analysis_height", kOwner);
  ai.edge.feather_px               = ReadRequiredFloat(json, "feather_px", kOwner);
  if (!json.contains("expand_px") || !json["expand_px"].is_number_integer()) {
    Fail("ai_segmentation expand_px must be an integer");
  }
  const auto expand = json["expand_px"].get<std::int64_t>();
  if (expand < -kAiMaskMaximumExpandPx || expand > kAiMaskMaximumExpandPx) {
    Fail("AI Mask expand_px must be in [-50, 50]");
  }
  ai.edge.expand_px       = static_cast<std::int32_t>(expand);
  ai.model_set_id         = ReadRequiredString(json, "model_set_id", kOwner);
  ai.geometry_fingerprint = ReadRequiredString(json, "geometry_fingerprint", kOwner);
  if (!json.contains("components")) {
    Fail("ai_segmentation is missing components");
  }
  ai.recipe = AiMaskRecipeFromJson(json["components"]);
  return ai;
}

auto SourceToJsonUnchecked(const MaskSource& mask_source) -> nlohmann::json {
  nlohmann::json source;
  std::visit(
      [&source](const auto& value) {
        using Source = std::decay_t<decltype(value)>;
#ifdef ALCEDO_ENABLE_BRUSH_MASK
        if constexpr (std::is_same_v<Source, BrushMaskSource>) {
          source = BrushToJson(value);
        } else
#endif
            if constexpr (std::is_same_v<Source, RadialMaskSource>) {
          source = RadialToJson(value);
        } else if constexpr (std::is_same_v<Source, LinearGradientMaskSource>) {
          source = LinearGradientToJson(value);
        } else {
          static_assert(std::is_same_v<Source, AiSegmentationMaskSource>,
                        "SourceToJsonUnchecked misses a Mask source kind");
          source = AiSegmentationToJson(value);
        }
      },
      mask_source);
  return source;
}

/// Read one source object by its `kind`. Does not run the value rules of @ref ValidateMaskSource.
auto SourceFromJsonUnchecked(const nlohmann::json& source_json) -> MaskSource {
  if (!source_json.contains("kind") || !source_json["kind"].is_string()) {
    Fail("Mask source is missing kind");
  }
  const auto kind = source_json["kind"].get<std::string>();
#ifdef ALCEDO_ENABLE_BRUSH_MASK
  if (kind == "brush") {
    return BrushFromJson(source_json);
  }
#endif
  if (kind == "radial") {
    return RadialFromJson(source_json);
  }
  if (kind == "linear_gradient") {
    return LinearGradientFromJson(source_json);
  }
  if (kind == "ai_segmentation") {
    return AiSegmentationFromJson(source_json);
  }
  Fail("Unknown Mask source kind: " + kind);
}

/// Whether @p json is the pre-range placeholder `{"enabled": false}`, which reads as empty.
auto IsLegacyRangePlaceholder(const nlohmann::json& json, std::string_view name) -> bool {
  if (!json.contains("enabled")) {
    return false;
  }
  RequireOnlyKeys(json, name, {"enabled"});
  if (!json["enabled"].is_boolean()) {
    Fail(std::string{name} + " enabled must be boolean");
  }
  if (json["enabled"].get<bool>()) {
    Fail(std::string{name} + " cannot be enabled");
  }
  return true;
}

void RequireColorRangeSoftness(float softness) {
  RequireFinite(softness, "color_range softness");
  if (!(softness > 0.0f) || softness > kColorRangeMaximumSoftnessDegrees) {
    Fail("color_range softness must be in (0, 90]");
  }
}

void RequireColorRangeMinChroma(float min_chroma) {
  RequireFinite(min_chroma, "color_range min_chroma");
  if (min_chroma < kColorRangeMinimumMinChroma || min_chroma > kColorRangeMaximumMinChroma) {
    Fail("color_range min_chroma must be in [0.015, 0.2]");
  }
}

void RequireLuminanceRangeSoftness(float softness) {
  RequireFinite(softness, "luminance_range softness");
  if (!(softness > 0.0f) || softness > kLuminanceRangeMaximumSoftness) {
    Fail("luminance_range softness must be in (0, 0.5]");
  }
}

void RequireUnitInterval(float value, std::string_view name) {
  RequireFinite(value, name);
  if (value < 0.0f || value > 1.0f) {
    Fail(std::string{name} + " must be in [0, 1]");
  }
}

}  // namespace

#ifdef ALCEDO_ENABLE_BRUSH_MASK
auto BrushSourceHasParameterizedPayload(const BrushMaskSource& brush) -> bool {
  return !brush.strokes.empty() || brush.placement_translation.x != 0.0f ||
         brush.placement_translation.y != 0.0f;
}
#endif

void ValidateAiSegmentationMaskSource(const AiSegmentationMaskSource& source) {
  ValidateAiSegmentation(source);
}

auto ColorRangeWidthDegrees(const ColorRangeModel& range) -> float {
  if (range.hue_start <= range.hue_end) {
    return range.hue_end - range.hue_start;
  }
  return 360.0f - range.hue_start + range.hue_end;
}

auto IsFullColorRange(const ColorRangeModel& range) -> bool {
  return range.hue_start == 0.0f && range.hue_end == 360.0f;
}

auto IsFullLuminanceRange(const LuminanceRangeModel& range) -> bool {
  return range.low == 0.0f && range.high == 1.0f;
}

void ValidateColorRange(const ColorRangeModel& range) {
  RequireFinite(range.hue_start, "color_range hue_start");
  RequireFinite(range.hue_end, "color_range hue_end");
  if (range.hue_start < 0.0f || range.hue_start >= 360.0f) {
    Fail("color_range hue_start must be in [0, 360)");
  }
  if (range.hue_end <= 0.0f || range.hue_end > 360.0f) {
    Fail("color_range hue_end must be in (0, 360]");
  }
  if (ColorRangeWidthDegrees(range) < kColorRangeMinimumWidthDegrees) {
    Fail("color_range must select at least 1 degree");
  }
  RequireColorRangeSoftness(range.softness);
  RequireColorRangeMinChroma(range.min_chroma);
  if (IsFullColorRange(range)) {
    Fail("color_range covers every hue; a full range is stored as null");
  }
}

void ValidateLuminanceRange(const LuminanceRangeModel& range) {
  RequireUnitInterval(range.low, "luminance_range low");
  RequireUnitInterval(range.high, "luminance_range high");
  if (range.high - range.low < kLuminanceRangeMinimumWidth) {
    Fail("luminance_range high - low must be at least 0.01");
  }
  RequireLuminanceRangeSoftness(range.softness);
  if (IsFullLuminanceRange(range)) {
    Fail("luminance_range covers every luminance; a full range is stored as null");
  }
}

auto ColorRangeToJson(const std::optional<ColorRangeModel>& range) -> nlohmann::json {
  if (!range.has_value()) {
    return nullptr;
  }
  nlohmann::json json = {
      {"hue_start", range->hue_start}, {"hue_end", range->hue_end}, {"softness", range->softness}};
  if (range->min_chroma != kColorRangeDefaultMinChroma) {
    json["min_chroma"] = range->min_chroma;
  }
  return json;
}

auto LuminanceRangeToJson(const std::optional<LuminanceRangeModel>& range) -> nlohmann::json {
  if (!range.has_value()) {
    return nullptr;
  }
  return {{"low", range->low}, {"high", range->high}, {"softness", range->softness}};
}

auto ColorRangeFromJson(const nlohmann::json& json) -> std::optional<ColorRangeModel> {
  constexpr std::string_view kName = "color_range";
  if (json.is_null()) {
    return std::nullopt;
  }
  RequireObject(json, kName);
  if (IsLegacyRangePlaceholder(json, kName)) {
    return std::nullopt;
  }
  RequireOnlyKeys(json, kName, {"hue_start", "hue_end", "softness", "min_chroma"});
  ColorRangeModel range;
  range.hue_start = ReadRequiredFloat(json, "hue_start", kName);
  range.hue_end   = ReadRequiredFloat(json, "hue_end", kName);
  range.softness  = ReadRequiredFloat(json, "softness", kName);
  if (json.contains("min_chroma")) {
    range.min_chroma = ReadRequiredFloat(json, "min_chroma", kName);
  }
  if (IsFullColorRange(range)) {
    RequireColorRangeSoftness(range.softness);
    RequireColorRangeMinChroma(range.min_chroma);
    return std::nullopt;
  }
  ValidateColorRange(range);
  return range;
}

auto LuminanceRangeFromJson(const nlohmann::json& json) -> std::optional<LuminanceRangeModel> {
  constexpr std::string_view kName = "luminance_range";
  if (json.is_null()) {
    return std::nullopt;
  }
  RequireObject(json, kName);
  if (IsLegacyRangePlaceholder(json, kName)) {
    return std::nullopt;
  }
  RequireOnlyKeys(json, kName, {"low", "high", "softness"});
  LuminanceRangeModel range;
  range.low      = ReadRequiredFloat(json, "low", kName);
  range.high     = ReadRequiredFloat(json, "high", kName);
  range.softness = ReadRequiredFloat(json, "softness", kName);
  if (IsFullLuminanceRange(range)) {
    RequireLuminanceRangeSoftness(range.softness);
    return std::nullopt;
  }
  ValidateLuminanceRange(range);
  return range;
}

auto GetMaskSourceKind(const MaskSource& source) -> MaskSourceKind {
#ifdef ALCEDO_ENABLE_BRUSH_MASK
  if (std::holds_alternative<BrushMaskSource>(source)) {
    return MaskSourceKind::Brush;
  }
#endif
  if (std::holds_alternative<RadialMaskSource>(source)) {
    return MaskSourceKind::Radial;
  }
  if (std::holds_alternative<LinearGradientMaskSource>(source)) {
    return MaskSourceKind::LinearGradient;
  }
  if (std::holds_alternative<AiSegmentationMaskSource>(source)) {
    return MaskSourceKind::AiSegmentation;
  }
  Fail("Mask source holds no value");
}

auto MaskSourceKindText(MaskSourceKind kind) -> std::string_view {
  switch (kind) {
#ifdef ALCEDO_ENABLE_BRUSH_MASK
    case MaskSourceKind::Brush:
      return "brush";
#endif
    case MaskSourceKind::Radial:
      return "radial";
    case MaskSourceKind::LinearGradient:
      return "linear_gradient";
    case MaskSourceKind::AiSegmentation:
      return "ai_segmentation";
  }
  return "unknown";
}

void ValidateMaskSource(const MaskSource& source) {
  std::visit(
      [](const auto& value) {
        using Source = std::decay_t<decltype(value)>;
#ifdef ALCEDO_ENABLE_BRUSH_MASK
        if constexpr (std::is_same_v<Source, BrushMaskSource>) {
          ValidateBrush(value);
        } else
#endif
            if constexpr (std::is_same_v<Source, RadialMaskSource>) {
          ValidateRadial(value);
        } else if constexpr (std::is_same_v<Source, LinearGradientMaskSource>) {
          ValidateLinearGradient(value);
        } else {
          static_assert(std::is_same_v<Source, AiSegmentationMaskSource>,
                        "ValidateMaskSource misses a Mask source kind");
          ValidateAiSegmentation(value);
        }
      },
      source);
}

void ValidateMaskModel(const MaskModel& mask) {
  if (mask.id.Empty()) {
    Fail("MaskId must not be empty");
  }
  RequireFinite(mask.opacity, "opacity");
  if (mask.opacity < 0.0f || mask.opacity > 1.0f) {
    Fail("opacity must stay in [0, 1]");
  }
  ValidateMaskSource(mask.source);
  if (mask.color_range.has_value()) {
    ValidateColorRange(*mask.color_range);
  }
  if (mask.luminance_range.has_value()) {
    ValidateLuminanceRange(*mask.luminance_range);
  }
}

auto MaskSourceToJson(const MaskSource& source) -> nlohmann::json {
  ValidateMaskSource(source);
  return SourceToJsonUnchecked(source);
}

auto MaskSourceFromJson(const nlohmann::json& json) -> MaskSource {
  RequireObject(json, "Mask source");
  auto source = SourceFromJsonUnchecked(json);
  ValidateMaskSource(source);
  return source;
}

auto MaskModelToJson(const MaskModel& mask) -> nlohmann::json {
  ValidateMaskModel(mask);
  return {{"id", std::string{mask.id.Value()}},
          {"display_name", mask.display_name},
          {"enabled", mask.enabled},
          {"deletion_protected", mask.deletion_protected},
          {"opacity", mask.opacity},
          {"invert", mask.invert},
          {"source", SourceToJsonUnchecked(mask.source)},
          {"color_range", ColorRangeToJson(mask.color_range)},
          {"luminance_range", LuminanceRangeToJson(mask.luminance_range)}};
}

auto MaskModelFromJson(const nlohmann::json& json) -> MaskModel {
  RequireObject(json, "Mask");
  if (!json.contains("id") || !json["id"].is_string() || json["id"].get<std::string>().empty()) {
    Fail("Mask is missing a non-empty id");
  }
  if (!json.contains("source") || !json["source"].is_object()) {
    Fail("Mask is missing an object source");
  }
  if (!json.contains("color_range")) {
    Fail("Mask is missing color_range");
  }
  if (!json.contains("luminance_range")) {
    Fail("Mask is missing luminance_range");
  }
  if (!json.contains("deletion_protected") || !json.at("deletion_protected").is_boolean()) {
    Fail("Mask deletion_protected must be boolean");
  }
  MaskModel mask;
  mask.id                 = MaskId{json["id"].get<std::string>()};
  mask.display_name       = json.value("display_name", std::string{});
  mask.enabled            = json.value("enabled", true);
  mask.deletion_protected = json.at("deletion_protected").get<bool>();
  mask.opacity            = json.value("opacity", 1.0f);
  mask.invert             = json.value("invert", false);
  mask.source             = SourceFromJsonUnchecked(json["source"]);
  mask.color_range        = ColorRangeFromJson(json["color_range"]);
  mask.luminance_range    = LuminanceRangeFromJson(json["luminance_range"]);
  ValidateMaskModel(mask);
  return mask;
}

auto HasDuplicateOrEmptyMaskId(const std::vector<MaskModel>& masks) -> bool {
  std::unordered_set<std::string> seen;
  for (const auto& mask : masks) {
    if (mask.id.Empty()) {
      return true;
    }
    if (!seen.insert(std::string{mask.id.Value()}).second) {
      return true;
    }
  }
  return false;
}

auto FirstEnabledMask(MaskListView masks) -> const MaskModel* {
  for (const auto& mask : masks) {
    if (mask.enabled) {
      return &mask;
    }
  }
  return nullptr;
}

auto AnalyticKindFromMask(const MaskModel& mask) -> AnalyticMaskKind {
  if (std::holds_alternative<RadialMaskSource>(mask.source)) {
    return AnalyticMaskKind::Radial;
  }
  if (std::holds_alternative<LinearGradientMaskSource>(mask.source)) {
    return AnalyticMaskKind::LinearGradient;
  }
  throw std::invalid_argument("AnalyticKindFromMask: Mask source " +
                              std::string{MaskSourceKindText(GetMaskSourceKind(mask.source))} +
                              " is not analytic");
}

auto RadialParamsFromMask(const MaskModel& mask) -> RadialMaskParams {
  RadialMaskParams params;
  if (const auto* radial = std::get_if<RadialMaskSource>(&mask.source)) {
    params.center_x      = radial->center_x;
    params.center_y      = radial->center_y;
    params.major_radius  = radial->major_radius;
    params.minor_radius  = radial->minor_radius;
    params.rotation      = radial->rotation;
    params.inner_feather = radial->inner_feather;
    params.outer_feather = radial->outer_feather;
  }
  params.invert  = mask.invert;
  params.opacity = mask.opacity;
  return params;
}

auto LinearGradientParamsFromMask(const MaskModel& mask) -> LinearGradientMaskParams {
  LinearGradientMaskParams params;
  if (const auto* gradient = std::get_if<LinearGradientMaskSource>(&mask.source)) {
    params.origin_x            = gradient->origin_x;
    params.origin_y            = gradient->origin_y;
    params.normal_x            = gradient->normal_x;
    params.normal_y            = gradient->normal_y;
    params.transition_distance = gradient->transition_distance;
    params.start_value         = gradient->start_value;
    params.end_value           = gradient->end_value;
  }
  params.invert  = mask.invert;
  params.opacity = mask.opacity;
  return params;
}

auto MaskHasRestrictedRange(const MaskModel& mask) -> bool {
  return mask.luminance_range.has_value() || mask.color_range.has_value();
}

auto MaskRangeParamsFromMask(const MaskModel& mask) -> MaskRangeParams {
  MaskRangeParams params;
  if (mask.luminance_range.has_value()) {
    params.luminance_restricted = 1;
    params.luminance_low        = mask.luminance_range->low;
    params.luminance_high       = mask.luminance_range->high;
    params.luminance_softness   = mask.luminance_range->softness;
  }
  if (mask.color_range.has_value()) {
    params.hue_restricted = 1;
    params.hue_start      = mask.color_range->hue_start;
    params.hue_end        = mask.color_range->hue_end;
    params.hue_softness   = mask.color_range->softness;
    params.hue_min_chroma = mask.color_range->min_chroma;
  }
  return params;
}

}  // namespace alcedo
