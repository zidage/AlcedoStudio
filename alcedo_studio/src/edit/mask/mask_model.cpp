//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/mask/mask_model.hpp"

#include <cmath>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>

namespace alcedo {
namespace {

constexpr float kMinLinearGradientNormalLength = 1.0e-6f;

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

auto ReadRequiredFloat(const nlohmann::json& json, const char* key, std::string_view owner)
    -> float {
  RequireNumber(json, key, owner);
  const auto value = json[key].get<float>();
  RequireFinite(value, key);
  return value;
}

auto BoundsToJson(NormalizedRect bounds) -> nlohmann::json {
  return nlohmann::json::array({bounds.x, bounds.y, bounds.w, bounds.h});
}

auto BoundsFromJson(const nlohmann::json& json, std::string_view owner) -> NormalizedRect {
  if (!json.is_array() || json.size() < 4) {
    Fail(std::string{owner} + " reference_bounds must be an array of four numbers");
  }
  NormalizedRect bounds;
  bounds.x = json[0].get<float>();
  bounds.y = json[1].get<float>();
  bounds.w = json[2].get<float>();
  bounds.h = json[3].get<float>();
  RequireFinite(bounds.x, "reference_bounds.x");
  RequireFinite(bounds.y, "reference_bounds.y");
  RequireFinite(bounds.w, "reference_bounds.w");
  RequireFinite(bounds.h, "reference_bounds.h");
  return bounds;
}

void ValidateRangeObject(const nlohmann::json& json, std::string_view name) {
  RequireObject(json, name);
  for (const auto& [key, value] : json.items()) {
    if (key != "enabled") {
      Fail(std::string{name} + " contains unsupported field " + key);
    }
  }
  if (!json.contains("enabled") || !json["enabled"].is_boolean()) {
    Fail(std::string{name} + " must contain boolean enabled");
  }
  if (json["enabled"].get<bool>()) {
    Fail(std::string{name} + " cannot be enabled");
  }
}

void ValidateBrush(const BrushMaskSource& brush) {
  RequireNonNegative(brush.feather_radius, "feather_radius");
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

void ValidateRange(const std::optional<ColorRangeModel>& range, std::string_view name) {
  if (!range.has_value()) {
    return;
  }
  if (range->enabled) {
    Fail(std::string{name} + " cannot be enabled");
  }
}

void ValidateRange(const std::optional<LuminanceRangeModel>& range, std::string_view name) {
  if (!range.has_value()) {
    return;
  }
  if (range->enabled) {
    Fail(std::string{name} + " cannot be enabled");
  }
}

auto BrushToJson(const BrushMaskSource& brush) -> nlohmann::json {
  nlohmann::json json{{"kind", "brush"},
                      {"feather_radius", brush.feather_radius},
                      {"width", brush.descriptor.extent.width},
                      {"height", brush.descriptor.extent.height},
                      {"reference_bounds", BoundsToJson(brush.descriptor.reference_bounds)}};
  if (brush.asset_key.has_value() && !brush.asset_key->Empty()) {
    json["asset_key"] = std::string{brush.asset_key->Value()};
  } else {
    json["asset_key"] = nullptr;
  }
  return json;
}

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

auto RangeToJson(const std::optional<ColorRangeModel>& range) -> nlohmann::json {
  if (!range.has_value()) {
    return nullptr;
  }
  return {{"enabled", range->enabled}};
}

auto RangeToJson(const std::optional<LuminanceRangeModel>& range) -> nlohmann::json {
  if (!range.has_value()) {
    return nullptr;
  }
  return {{"enabled", range->enabled}};
}

auto BrushFromJson(const nlohmann::json& json) -> BrushMaskSource {
  BrushMaskSource brush;
  brush.feather_radius = ReadRequiredFloat(json, "feather_radius", "brush");
  if (!json.contains("width") || !json["width"].is_number()) {
    Fail("brush is missing number width");
  }
  if (!json.contains("height") || !json["height"].is_number()) {
    Fail("brush is missing number height");
  }
  brush.descriptor.extent.width  = json["width"].get<std::uint32_t>();
  brush.descriptor.extent.height = json["height"].get<std::uint32_t>();
  if (!json.contains("reference_bounds")) {
    Fail("brush is missing reference_bounds");
  }
  brush.descriptor.reference_bounds = BoundsFromJson(json["reference_bounds"], "brush");
  if (!json.contains("asset_key") || json["asset_key"].is_null()) {
    brush.asset_key.reset();
    return brush;
  }
  if (!json["asset_key"].is_string()) {
    Fail("brush asset_key must be a string or null");
  }
  auto key = json["asset_key"].get<std::string>();
  if (key.empty()) {
    brush.asset_key.reset();
  } else {
    brush.asset_key = MaskAssetKey{std::move(key)};
  }
  return brush;
}

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

auto ColorRangeFromJson(const nlohmann::json& json) -> std::optional<ColorRangeModel> {
  if (json.is_null()) {
    return std::nullopt;
  }
  ValidateRangeObject(json, "color_range");
  return ColorRangeModel{false};
}

auto LuminanceRangeFromJson(const nlohmann::json& json) -> std::optional<LuminanceRangeModel> {
  if (json.is_null()) {
    return std::nullopt;
  }
  ValidateRangeObject(json, "luminance_range");
  return LuminanceRangeModel{false};
}

}  // namespace

auto GetMaskSourceKind(const MaskSource& source) -> MaskSourceKind {
  if (std::holds_alternative<BrushMaskSource>(source)) {
    return MaskSourceKind::Brush;
  }
  if (std::holds_alternative<RadialMaskSource>(source)) {
    return MaskSourceKind::Radial;
  }
  return MaskSourceKind::LinearGradient;
}

auto MaskSourceKindText(MaskSourceKind kind) -> std::string_view {
  switch (kind) {
    case MaskSourceKind::Brush:
      return "brush";
    case MaskSourceKind::Radial:
      return "radial";
    case MaskSourceKind::LinearGradient:
      return "linear_gradient";
  }
  return "unknown";
}

void ValidateMaskModel(const MaskModel& mask) {
  if (mask.id.Empty()) {
    Fail("MaskId must not be empty");
  }
  RequireFinite(mask.opacity, "opacity");
  if (mask.opacity < 0.0f || mask.opacity > 1.0f) {
    Fail("opacity must stay in [0, 1]");
  }
  std::visit(
      [](const auto& source) {
        using Source = std::decay_t<decltype(source)>;
        if constexpr (std::is_same_v<Source, BrushMaskSource>) {
          ValidateBrush(source);
        } else if constexpr (std::is_same_v<Source, RadialMaskSource>) {
          ValidateRadial(source);
        } else {
          ValidateLinearGradient(source);
        }
      },
      mask.source);
  ValidateRange(mask.color_range, "color_range");
  ValidateRange(mask.luminance_range, "luminance_range");
}

auto MaskModelToJson(const MaskModel& mask) -> nlohmann::json {
  ValidateMaskModel(mask);
  nlohmann::json source;
  std::visit(
      [&source](const auto& value) {
        using Source = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Source, BrushMaskSource>) {
          source = BrushToJson(value);
        } else if constexpr (std::is_same_v<Source, RadialMaskSource>) {
          source = RadialToJson(value);
        } else {
          source = LinearGradientToJson(value);
        }
      },
      mask.source);
  return {{"id", std::string{mask.id.Value()}},
          {"display_name", mask.display_name},
          {"enabled", mask.enabled},
          {"opacity", mask.opacity},
          {"invert", mask.invert},
          {"source", std::move(source)},
          {"color_range", RangeToJson(mask.color_range)},
          {"luminance_range", RangeToJson(mask.luminance_range)}};
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
  const auto& source_json = json["source"];
  if (!source_json.contains("kind") || !source_json["kind"].is_string()) {
    Fail("Mask source is missing kind");
  }
  const auto kind = source_json["kind"].get<std::string>();
  MaskModel  mask;
  mask.id           = MaskId{json["id"].get<std::string>()};
  mask.display_name = json.value("display_name", std::string{});
  mask.enabled      = json.value("enabled", true);
  mask.opacity      = json.value("opacity", 1.0f);
  mask.invert       = json.value("invert", false);
  if (kind == "brush") {
    mask.source = BrushFromJson(source_json);
  } else if (kind == "radial") {
    mask.source = RadialFromJson(source_json);
  } else if (kind == "linear_gradient") {
    mask.source = LinearGradientFromJson(source_json);
  } else {
    Fail("Unknown Mask source kind: " + kind);
  }
  mask.color_range      = ColorRangeFromJson(json["color_range"]);
  mask.luminance_range  = LuminanceRangeFromJson(json["luminance_range"]);
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

auto FirstEnabledMask(std::span<const MaskModel> masks) -> const MaskModel* {
  for (const auto& mask : masks) {
    if (mask.enabled) {
      return &mask;
    }
  }
  return nullptr;
}

auto AnalyticKindFromMask(const MaskModel& mask) -> AnalyticMaskKind {
  return std::holds_alternative<RadialMaskSource>(mask.source) ? AnalyticMaskKind::Radial
                                                               : AnalyticMaskKind::LinearGradient;
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

}  // namespace alcedo
