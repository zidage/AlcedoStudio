//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_parameter_write.hpp"

#include <array>
#include <cmath>
#include <exception>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "app/dcp_camera_profile_binding.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/film_format.hpp"
#include "edit/graph/image_geometry_model.hpp"
#include "edit/operators/models/cat02_white_balance_model.hpp"
#include "edit/operators/models/color_wheel_model.hpp"
#include "edit/operators/models/curve_model.hpp"
#include "edit/operators/models/hls_model.hpp"
#include "edit/operators/models/sharpen_model.hpp"
#include "edit/operators/models/tone_curve_presets.hpp"
#include "edit/operators/models/tone_curve_spline.hpp"

namespace alcedo {
namespace {


auto SetError(std::string* error, std::string message) -> bool {
  if (error != nullptr) {
    *error = std::move(message);
  }
  return false;
}

auto IsAllowedKey(std::string_view key, std::initializer_list<std::string_view> allowed) -> bool {
  for (const auto candidate : allowed) {
    if (key == candidate) {
      return true;
    }
  }
  return false;
}

void RejectUnknownKeys(const nlohmann::json&                   object,
                       std::initializer_list<std::string_view> allowed, std::string_view context) {
  if (!object.is_object()) {
    throw std::invalid_argument(std::string{context} + " must be an object");
  }
  for (const auto& [key, value] : object.items()) {
    static_cast<void>(value);
    if (!IsAllowedKey(key, allowed)) {
      throw std::invalid_argument(std::string{"Unknown parameter: "} + std::string{context} + "." +
                                  key);
    }
  }
}

auto RequireObject(const nlohmann::json& value, std::string_view context) -> const nlohmann::json& {
  if (!value.is_object()) {
    throw std::invalid_argument(std::string{context} + " must be an object");
  }
  return value;
}

auto UnwrapObject(const nlohmann::json& params, std::initializer_list<std::string_view> wrappers,
                  std::string_view context) -> const nlohmann::json& {
  RequireObject(params, context);
  const nlohmann::json* nested = nullptr;
  std::string           wrapper_name;
  for (const auto wrapper : wrappers) {
    if (!params.contains(wrapper)) {
      continue;
    }
    if (nested != nullptr) {
      throw std::invalid_argument(std::string{context} + " contains multiple wrappers");
    }
    nested       = &params.at(std::string{wrapper});
    wrapper_name = std::string{wrapper};
  }
  if (nested == nullptr) {
    return params;
  }
  if (params.size() != 1) {
    throw std::invalid_argument(std::string{"Unknown parameter: "} + std::string{context} + "." +
                                wrapper_name);
  }
  return RequireObject(*nested, std::string{context} + "." + wrapper_name);
}

template <typename ValueReader>
auto ReadOptionalValue(const nlohmann::json& object, std::initializer_list<std::string_view> keys,
                       std::string_view context, ValueReader&& reader)
    -> std::optional<decltype(reader(std::declval<const nlohmann::json&>(),
                                     std::declval<std::string_view>()))> {
  const nlohmann::json* value = nullptr;
  std::string_view      selected;
  for (const auto key : keys) {
    if (object.contains(key)) {
      if (value != nullptr && selected == key) {
        continue;
      }
      if (value != nullptr) {
        throw std::invalid_argument(std::string{context} + " contains multiple aliases");
      }
      value    = &object.at(std::string{key});
      selected = key;
    }
  }
  if (value == nullptr) {
    using Result =
        decltype(reader(std::declval<const nlohmann::json&>(), std::declval<std::string_view>()));
    return std::optional<Result>{};
  }
  return std::optional<decltype(reader(std::declval<const nlohmann::json&>(),
                                       std::declval<std::string_view>()))>{
      reader(*value, std::string{context} + "." + std::string{selected})};
}

auto ReadFiniteFloat(const nlohmann::json& value, std::string_view context) -> float {
  if (!value.is_number()) {
    throw std::invalid_argument(std::string{context} + " must be a finite number");
  }
  const double as_double = value.get<double>();
  const float  as_float  = static_cast<float>(as_double);
  if (!std::isfinite(as_double) || !std::isfinite(as_float)) {
    throw std::invalid_argument(std::string{context} + " must be a finite number");
  }
  return as_float;
}

auto ReadBoolean(const nlohmann::json& value, std::string_view context) -> bool {
  if (!value.is_boolean()) {
    throw std::invalid_argument(std::string{context} + " must be a boolean");
  }
  return value.get<bool>();
}

auto ReadString(const nlohmann::json& value, std::string_view context) -> std::string {
  if (!value.is_string()) {
    throw std::invalid_argument(std::string{context} + " must be a string");
  }
  return value.get<std::string>();
}

auto ReadOptionalFloat(const nlohmann::json& object, std::initializer_list<std::string_view> keys,
                       std::string_view context) -> std::optional<float> {
  return ReadOptionalValue(object, keys, context,
                           [](const nlohmann::json& value, std::string_view key_context) {
                             return ReadFiniteFloat(value, key_context);
                           });
}

auto ReadOptionalBool(const nlohmann::json& object, std::initializer_list<std::string_view> keys,
                      std::string_view context) -> std::optional<bool> {
  return ReadOptionalValue(object, keys, context,
                           [](const nlohmann::json& value, std::string_view key_context) {
                             return ReadBoolean(value, key_context);
                           });
}

auto ReadOptionalString(const nlohmann::json& object, std::initializer_list<std::string_view> keys,
                        std::string_view context) -> std::optional<std::string> {
  return ReadOptionalValue(object, keys, context,
                           [](const nlohmann::json& value, std::string_view key_context) {
                             return ReadString(value, key_context);
                           });
}

auto ParseScalarWrite(const nlohmann::json& params, std::string_view field,
                      std::string_view canonical_key) -> EditorScalarWrite {
  const auto& object = (params.contains(field) && params.at(std::string{field}).is_object())
                           ? UnwrapObject(params, {field}, field)
                           : RequireObject(params, field);
  RejectUnknownKeys(object, {canonical_key, field, "value"}, field);
  const auto value = ReadOptionalFloat(object, {canonical_key, field, "value"}, field);
  if (!value.has_value()) {
    throw std::invalid_argument(std::string{field} + " requires a numeric value");
  }
  return EditorScalarWrite{*value};
}

/// The form of a parse: a write sets only its keys; a complete value sets every field.
enum class ParseForm { Write, Value };

/// The control points of one curve under `curve.<key>`. They lie in the unit square and their x
/// values strictly increase, as the curve panel writes them. Out-of-range or unordered points are
/// rejected, not clamped or sorted.
auto ParseCurvePoints(const nlohmann::json& points_json, std::string_view key)
    -> std::vector<CurvePoint> {
  const std::string name = "curve." + std::string{key};
  if (!points_json.is_array() || points_json.size() < tone_curve::kMinControlPoints) {
    throw std::invalid_argument(name + " must contain at least two points");
  }
  if (points_json.size() > tone_curve::kMaxControlPoints) {
    throw std::invalid_argument(name + " must contain at most " +
                                std::to_string(tone_curve::kMaxControlPoints) + " points");
  }
  std::vector<CurvePoint> points;
  points.reserve(points_json.size());
  for (std::size_t index = 0; index < points_json.size(); ++index) {
    const auto  context = name + "[" + std::to_string(index) + "]";
    const auto& point   = RequireObject(points_json.at(index), context);
    RejectUnknownKeys(point, {"x", "y"}, "curve point");
    if (!point.contains("x") || !point.contains("y")) {
      throw std::invalid_argument("curve points require x and y");
    }
    const CurvePoint parsed{ReadFiniteFloat(point.at("x"), context + ".x"),
                            ReadFiniteFloat(point.at("y"), context + ".y")};
    if (parsed.x < 0.0f || parsed.x > 1.0f) {
      throw std::invalid_argument(context + ".x must be in [0, 1]");
    }
    if (parsed.y < 0.0f || parsed.y > 1.0f) {
      throw std::invalid_argument(context + ".y must be in [0, 1]");
    }
    if (!points.empty() && parsed.x <= points.back().x) {
      throw std::invalid_argument(context + ".x must be greater than " + name + "[" +
                                  std::to_string(index - 1) + "].x");
    }
    points.push_back(parsed);
  }
  return points;
}

/// The points of the Tone Curve preset @p id_json for `curve.<key>` (a write only).
auto ParseCurvePresetPoints(const nlohmann::json& id_json, std::string_view key)
    -> std::vector<CurvePoint> {
  const auto  id     = id_json.get<std::string>();
  const auto* preset = FindToneCurvePreset(id);
  if (preset == nullptr) {
    throw std::invalid_argument("curve." + std::string{key} + " names no Tone Curve preset: " + id +
                                "; curve.presets lists them");
  }
  return preset->PointList();
}

/// `curve`: the RGB curve under `points` (with an optional `size`, the point count) and the
/// channel curves under `red`, `green`, and `blue`. A write has one or more of the four curves and
/// leaves the others as they are; the value of a curve is its list of points or, in a write, the
/// id of a Tone Curve preset, which writes the points of the preset. A complete value (the Model
/// JSON of history replay) has `points`; a channel key that it does not have is the identity, as
/// the Model omits it.
auto ParseCurveWrite(const nlohmann::json& params, ParseForm form) -> EditorCurveWrite {
  const auto& object = UnwrapObject(params, {"curve"}, "curve");
  RejectUnknownKeys(object, {"points", "size", "red", "green", "blue"}, "curve");
  if (form == ParseForm::Value && !object.contains("points")) {
    throw std::invalid_argument("curve.points is required");
  }
  if (form == ParseForm::Write && !object.contains("points") && !object.contains("red") &&
      !object.contains("green") && !object.contains("blue")) {
    throw std::invalid_argument("curve requires points, red, green, or blue");
  }
  if (object.contains("size")) {
    if (!object.contains("points") || !object.at("size").is_number_integer() ||
        !object.at("points").is_array() ||
        object.at("size").get<std::size_t>() != object.at("points").size()) {
      throw std::invalid_argument("curve.size must match curve.points");
    }
  }
  EditorCurveWrite write;
  for (const auto channel : kCurveChannels) {
    const std::string key{CurveChannelKey(channel)};
    if (object.contains(key) && form == ParseForm::Write && object.at(key).is_string()) {
      write.Channel(channel) = ParseCurvePresetPoints(object.at(key), key);
    } else if (object.contains(key)) {
      write.Channel(channel) = ParseCurvePoints(object.at(key), key);
    } else if (form == ParseForm::Value) {
      write.Channel(channel) = IdentityCurvePoints();
    }
  }
  return write;
}

/// Parse the complete LMT state: the Model JSON (`cube_path` or a tagged `reference`,
/// optional `name`, `strength`, `input_encoding` and `output_encoding`) or a legacy panel object
/// whose path is under `ocio_lmt`, `lut`, or `value`. A missing strength is the 100% default and
/// a missing encoding is ACEScc, so replaying a stored state (undo, redo, adjustment transfer)
/// restores it exactly.
auto ParseLutWrite(const nlohmann::json& params) -> EditorLutWrite {
  RequireObject(params, "lut");
  const auto& object = (params.contains("lut") && params.at("lut").is_object())
                           ? UnwrapObject(params, {"lut"}, "lut")
                       : (params.contains("ocio_lmt") && params.at("ocio_lmt").is_object())
                           ? UnwrapObject(params, {"ocio_lmt"}, "lut")
                           : params;
  RejectUnknownKeys(object,
                    {"cube_path", "ocio_lmt", "lut", "value", "reference", "name", "strength",
                     "input_encoding", "output_encoding"},
                    "lut");
  nlohmann::json model = nlohmann::json::object();
  for (const char* key : {"reference", "name", "strength", "input_encoding", "output_encoding"}) {
    if (object.contains(key)) model[key] = object.at(key);
  }
  if (const auto path =
          ReadOptionalString(object, {"cube_path", "ocio_lmt", "lut", "value"}, "lut")) {
    model["cube_path"] = *path;
  }
  return LmtUpdateFromModelJson(model);
}

auto ParseHlsVec3(const nlohmann::json& value, std::string_view context) -> HlsVec3 {
  if (!value.is_array() || value.size() != 3) {
    throw std::invalid_argument(std::string{context} + " must contain three numbers");
  }
  return {ReadFiniteFloat(value.at(0), std::string{context} + "[0]"),
          ReadFiniteFloat(value.at(1), std::string{context} + "[1]"),
          ReadFiniteFloat(value.at(2), std::string{context} + "[2]")};
}

template <typename Value>
auto ParseFixedNumberArray(const nlohmann::json& value, std::string_view context)
    -> std::array<Value, kHlsHueBinCount> {
  if (!value.is_array() || value.size() != kHlsHueBinCount) {
    throw std::invalid_argument(std::string{context} + " must contain eight values");
  }
  std::array<Value, kHlsHueBinCount> result{};
  for (std::size_t index = 0; index < result.size(); ++index) {
    if constexpr (std::is_same_v<Value, float>) {
      result[index] = ReadFiniteFloat(value.at(index),
                                      std::string{context} + "[" + std::to_string(index) + "]");
    } else {
      result[index] =
          ParseHlsVec3(value.at(index), std::string{context} + "[" + std::to_string(index) + "]");
    }
  }
  return result;
}

auto ParseHlsUpdate(const nlohmann::json& params) -> HlsUpdate {
  const auto& object = UnwrapObject(params, {"HLS", "hls"}, "HLS");
  RejectUnknownKeys(object,
                    {"hue_bins", "hls_adj_table", "h_range_table", "target_hls", "hls_adj",
                     "h_range", "l_range", "s_range"},
                    "HLS");
  HlsUpdate update;
  if (object.contains("hue_bins")) {
    update.hue_bins = ParseFixedNumberArray<float>(object.at("hue_bins"), "HLS.hue_bins");
  }
  if (object.contains("hls_adj_table")) {
    update.hls_adj_table =
        ParseFixedNumberArray<HlsVec3>(object.at("hls_adj_table"), "HLS.hls_adj_table");
  }
  if (object.contains("h_range_table")) {
    update.h_range_table =
        ParseFixedNumberArray<float>(object.at("h_range_table"), "HLS.h_range_table");
  }
  if (object.contains("target_hls")) {
    update.target_hls = ParseHlsVec3(object.at("target_hls"), "HLS.target_hls");
  }
  if (object.contains("hls_adj")) {
    update.hls_adj = ParseHlsVec3(object.at("hls_adj"), "HLS.hls_adj");
  }
  update.h_range = ReadOptionalFloat(object, {"h_range"}, "HLS");
  update.l_range = ReadOptionalFloat(object, {"l_range"}, "HLS");
  update.s_range = ReadOptionalFloat(object, {"s_range"}, "HLS");
  return update;
}

auto ParseVec2(const nlohmann::json& value, std::string_view context) -> Vec2f {
  const auto& object = RequireObject(value, context);
  RejectUnknownKeys(object, {"x", "y"}, context);
  if (!object.contains("x") || !object.contains("y")) {
    throw std::invalid_argument(std::string{context} + " requires x and y");
  }
  return {ReadFiniteFloat(object.at("x"), std::string{context} + ".x"),
          ReadFiniteFloat(object.at("y"), std::string{context} + ".y")};
}

auto ParseVec3(const nlohmann::json& value, std::string_view context) -> Vec3f {
  const auto& object = RequireObject(value, context);
  RejectUnknownKeys(object, {"x", "y", "z"}, context);
  if (!object.contains("x") || !object.contains("y") || !object.contains("z")) {
    throw std::invalid_argument(std::string{context} + " requires x, y, and z");
  }
  return {ReadFiniteFloat(object.at("x"), std::string{context} + ".x"),
          ReadFiniteFloat(object.at("y"), std::string{context} + ".y"),
          ReadFiniteFloat(object.at("z"), std::string{context} + ".z")};
}

auto ParseColorWheelControl(const nlohmann::json& value, std::string_view context)
    -> ColorWheelControlUpdate {
  const auto& object = RequireObject(value, context);
  RejectUnknownKeys(object, {"disc", "strength", "color_offset", "luminance_offset"}, context);
  ColorWheelControlUpdate update;
  if (object.contains("disc")) {
    update.disc = ParseVec2(object.at("disc"), std::string{context} + ".disc");
  }
  update.strength = ReadOptionalFloat(object, {"strength"}, context);
  if (object.contains("color_offset")) {
    update.color_offset =
        ParseVec3(object.at("color_offset"), std::string{context} + ".color_offset");
  }
  update.luminance_offset = ReadOptionalFloat(object, {"luminance_offset"}, context);
  return update;
}

auto ParseColorWheelUpdate(const nlohmann::json& params) -> ColorWheelUpdate {
  const auto& object = UnwrapObject(params, {"color_wheel"}, "color_wheel");
  RejectUnknownKeys(object, {"version", "lift", "gamma", "gain", "offset"}, "color_wheel");
  ColorWheelUpdate update;
  // JSON without a version is persisted format version 1: the complete ACEScc CDL JSON that an
  // older project stores (its root, its history values, and Model JSON read before an edit). The
  // Model converts it. Version 2 is a patch of the tone wheels.
  const auto       version = object.contains("version") ? object.at("version") : nlohmann::json(1);
  if (version == 1) {
    RejectUnknownKeys(object, {"version", "lift", "gamma", "gain"}, "color_wheel");
    nlohmann::json values = object;
    values.erase("version");
    update.version_one_json = std::move(values);
    return update;
  }
  if (version != 2) {
    throw std::invalid_argument("color_wheel.version must be 1 or 2");
  }
  if (object.contains("lift")) {
    update.lift = ParseColorWheelControl(object.at("lift"), "color_wheel.lift");
  }
  if (object.contains("gamma")) {
    update.gamma = ParseColorWheelControl(object.at("gamma"), "color_wheel.gamma");
  }
  if (object.contains("gain")) {
    update.gain = ParseColorWheelControl(object.at("gain"), "color_wheel.gain");
  }
  if (object.contains("offset")) {
    update.offset = ParseColorWheelControl(object.at("offset"), "color_wheel.offset");
  }
  return update;
}

auto ParseRawDecodeUpdate(const nlohmann::json& params) -> DevelopRawDecodeUpdate {
  const auto& object = UnwrapObject(params, {"raw", "raw_decode"}, "raw_decode");
  RejectUnknownKeys(object,
                    {"method",
                     "demosaic_method",
                     "highlights_reconstruct",
                     "use_camera_wb",
                     "user_wb",
                     "backend",
                     "decode_res",
                     "gpu_backend",
                     "wb_mode",
                     "custom_cct",
                     "custom_tint",
                     "as_shot_cct",
                     "as_shot_tint",
                     "camera_profile",
                     "camera_profile_file",
                     "lens_enabled",
                     "apply_vignetting",
                     "apply_distortion",
                     "apply_tca",
                     "apply_crop",
                     "auto_scale",
                     "use_user_scale",
                     "user_scale",
                     "projection_enabled",
                     "target_projection",
                     "lens_profile_db_path",
                     "lens_maker",
                     "lens_model"},
                    "raw_decode");
  DevelopRawDecodeUpdate update;
  update.demosaic_method = ReadOptionalString(object, {"demosaic_method", "method"}, "raw_decode");
  update.highlights_reconstruct =
      ReadOptionalBool(object, {"highlights_reconstruct"}, "raw_decode");
  update.use_camera_wb = ReadOptionalBool(object, {"use_camera_wb"}, "raw_decode");
  update.user_wb       = ReadOptionalFloat(object, {"user_wb"}, "raw_decode");
  return update;
}

auto ParseColorTemperatureUpdate(const nlohmann::json& params) -> DevelopColorTemperatureUpdate {
  const auto& object = UnwrapObject(params, {"color_temp"}, "color_temp");
  RejectUnknownKeys(object,
                    {"mode",
                     "wb_mode",
                     "custom_cct",
                     "custom_tint",
                     "cct",
                     "tint",
                     "as_shot_cct",
                     "as_shot_tint",
                     "resolved_cct",
                     "resolved_tint",
                     "demosaic_method",
                     "highlights_reconstruct",
                     "use_camera_wb",
                     "user_wb",
                     "camera_profile",
                     "camera_profile_file",
                     "lens_enabled",
                     "apply_vignetting",
                     "apply_distortion",
                     "apply_tca",
                     "apply_crop",
                     "auto_scale",
                     "use_user_scale",
                     "user_scale",
                     "projection_enabled",
                     "target_projection",
                     "lens_profile_db_path",
                     "lens_maker",
                     "lens_model"},
                    "color_temp");
  DevelopColorTemperatureUpdate update;
  update.wb_mode      = ReadOptionalString(object, {"wb_mode", "mode"}, "color_temp");
  update.custom_cct   = ReadOptionalFloat(object, {"custom_cct", "cct"}, "color_temp");
  update.custom_tint  = ReadOptionalFloat(object, {"custom_tint", "tint"}, "color_temp");
  update.as_shot_cct  = ReadOptionalFloat(object, {"as_shot_cct"}, "color_temp");
  update.as_shot_tint = ReadOptionalFloat(object, {"as_shot_tint"}, "color_temp");
  return update;
}

/// `input_profile`: {"profile_override": "<value>"}, or the same object under "input_profile",
/// or {"value": "<value>"}.
auto ParseInputProfileUpdate(const nlohmann::json& params) -> DevelopInputProfileUpdate {
  const auto&                object = UnwrapObject(params, {"input_profile"}, "input_profile");
  std::optional<std::string> value;
  if (object.contains("profile_override")) {
    value = ReadOptionalString(object, {"profile_override"}, "input_profile");
  } else {
    value = ReadOptionalString(object, {"value"}, "input_profile");
  }
  if (!value.has_value() || !IsRasterInputProfileOverride(*value)) {
    throw std::invalid_argument(
        "input_profile: profile_override must be one of the input profile "
        "values");
  }
  return DevelopInputProfileUpdate{std::move(*value)};
}

/// `{"film_format": "16mm"}`, or `{"value": "16mm"}` from an enum control.
auto ParseFilmFormatUpdate(const nlohmann::json& params) -> DrtFilmFormatUpdate {
  RequireObject(params, "film_format");
  RejectUnknownKeys(params, {"film_format", "value", "index"}, "film_format");
  auto value = ReadOptionalString(params, {"film_format"}, "film_format");
  if (!value.has_value()) {
    value = ReadOptionalString(params, {"value"}, "film_format");
  }
  const auto format = value.has_value() ? TryParseFilmFormat(*value) : std::optional<FilmFormat>{};
  if (!format.has_value()) {
    throw std::invalid_argument("film_format must be one of 16mm, 35mm, 70mm");
  }
  return DrtFilmFormatUpdate{*format};
}

auto ParseLensCalibrationUpdate(const nlohmann::json& params) -> DevelopLensCalibrationUpdate {
  const auto& object = UnwrapObject(params, {"lens_calib"}, "lens_calib");
  RejectUnknownKeys(object,
                    {"enabled",
                     "lens_enabled",
                     "apply_vignetting",
                     "apply_distortion",
                     "apply_tca",
                     "apply_crop",
                     "auto_scale",
                     "use_user_scale",
                     "user_scale",
                     "projection_enabled",
                     "target_projection",
                     "lens_profile_db_path",
                     "cam_maker",
                     "cam_model",
                     "lens_maker",
                     "lens_model",
                     "focal_length_mm",
                     "aperture_f_number",
                     "distance_m",
                     "focal_35mm_mm",
                     "crop_factor_hint",
                     "demosaic_method",
                     "highlights_reconstruct",
                     "use_camera_wb",
                     "user_wb",
                     "wb_mode",
                     "custom_cct",
                     "custom_tint",
                     "as_shot_cct",
                     "as_shot_tint",
                     "camera_profile",
                     "camera_profile_file"},
                    "lens_calib");
  DevelopLensCalibrationUpdate update;
  update.lens_enabled         = ReadOptionalBool(object, {"lens_enabled", "enabled"}, "lens_calib");
  update.apply_vignetting     = ReadOptionalBool(object, {"apply_vignetting"}, "lens_calib");
  update.apply_distortion     = ReadOptionalBool(object, {"apply_distortion"}, "lens_calib");
  update.apply_tca            = ReadOptionalBool(object, {"apply_tca"}, "lens_calib");
  update.apply_crop           = ReadOptionalBool(object, {"apply_crop"}, "lens_calib");
  update.auto_scale           = ReadOptionalBool(object, {"auto_scale"}, "lens_calib");
  update.use_user_scale       = ReadOptionalBool(object, {"use_user_scale"}, "lens_calib");
  update.user_scale           = ReadOptionalFloat(object, {"user_scale"}, "lens_calib");
  update.projection_enabled   = ReadOptionalBool(object, {"projection_enabled"}, "lens_calib");
  update.target_projection    = ReadOptionalString(object, {"target_projection"}, "lens_calib");
  update.lens_profile_db_path = ReadOptionalString(object, {"lens_profile_db_path"}, "lens_calib");
  update.lens_maker           = ReadOptionalString(object, {"lens_maker"}, "lens_calib");
  update.lens_model           = ReadOptionalString(object, {"lens_model"}, "lens_calib");
  return update;
}

auto ParseNormalizedRect(const nlohmann::json& value, std::string_view context) -> NormalizedRect {
  if (value.is_array()) {
    if (value.size() != 4) {
      throw std::invalid_argument(std::string{context} + " must contain four values");
    }
    return {ReadFiniteFloat(value.at(0), std::string{context} + "[0]"),
            ReadFiniteFloat(value.at(1), std::string{context} + "[1]"),
            ReadFiniteFloat(value.at(2), std::string{context} + "[2]"),
            ReadFiniteFloat(value.at(3), std::string{context} + "[3]")};
  }
  const auto& object = RequireObject(value, context);
  RejectUnknownKeys(object, {"x", "y", "w", "h"}, context);
  if (!object.contains("x") || !object.contains("y") || !object.contains("w") ||
      !object.contains("h")) {
    throw std::invalid_argument(std::string{context} + " requires x, y, w, and h");
  }
  return {ReadFiniteFloat(object.at("x"), std::string{context} + ".x"),
          ReadFiniteFloat(object.at("y"), std::string{context} + ".y"),
          ReadFiniteFloat(object.at("w"), std::string{context} + ".w"),
          ReadFiniteFloat(object.at("h"), std::string{context} + ".h")};
}

auto ParseCropAspectRatio(const nlohmann::json& value, std::string_view context)
    -> CropAspectRatio {
  CropAspectRatio ratio;
  if (value.is_array()) {
    if (value.size() != 2) {
      throw std::invalid_argument(std::string{context} + " must contain two values");
    }
    ratio.width  = ReadFiniteFloat(value.at(0), std::string{context} + "[0]");
    ratio.height = ReadFiniteFloat(value.at(1), std::string{context} + "[1]");
  } else {
    const auto& object = RequireObject(value, context);
    RejectUnknownKeys(object, {"width", "height"}, context);
    if (!object.contains("width") || !object.contains("height")) {
      throw std::invalid_argument(std::string{context} + " requires width and height");
    }
    ratio.width  = ReadFiniteFloat(object.at("width"), std::string{context} + ".width");
    ratio.height = ReadFiniteFloat(object.at("height"), std::string{context} + ".height");
  }
  if (ratio.width <= 0.0f || ratio.height <= 0.0f) {
    throw std::invalid_argument(std::string{context} + " must be positive");
  }
  return ratio;
}

auto ParseGeometryUpdate(const nlohmann::json& params) -> ImageGeometryUpdate {
  const auto& object = UnwrapObject(params, {"crop_rotate"}, "crop_rotate");
  // History commits replay stored ImageGeometryModel JSON through this parser. Models stored
  // before the crop-frame semantics carry `expand_to_fit`; the key has no meaning now and is read
  // as a known key so that history still loads.
  RejectUnknownKeys(object,
                    {"crop_rect", "rotation_degrees", "angle_degrees", "aspect_ratio_preset",
                     "aspect_preset", "aspect_ratio", "expand_to_fit"},
                    "crop_rotate");
  if (object.contains("expand_to_fit")) {
    static_cast<void>(ReadBoolean(object.at("expand_to_fit"), "crop_rotate.expand_to_fit"));
  }
  ImageGeometryUpdate update;
  if (object.contains("crop_rect")) {
    update.crop_rect = ParseNormalizedRect(object.at("crop_rect"), "crop_rotate.crop_rect");
  }
  update.rotation_degrees =
      ReadOptionalFloat(object, {"rotation_degrees", "angle_degrees"}, "crop_rotate");
  update.aspect_preset =
      ReadOptionalString(object, {"aspect_ratio_preset", "aspect_preset"}, "crop_rotate");
  if (object.contains("aspect_ratio")) {
    update.aspect_ratio =
        ParseCropAspectRatio(object.at("aspect_ratio"), "crop_rotate.aspect_ratio");
  }
  return update;
}

auto ParseSharpenUpdate(const nlohmann::json& params) -> SharpenUpdate {
  const auto& object = (params.contains("sharpen") && params.at("sharpen").is_object())
                           ? UnwrapObject(params, {"sharpen"}, "sharpen")
                           : RequireObject(params, "sharpen");
  RejectUnknownKeys(object, {"amount", "offset", "radius", "threshold"}, "sharpen");
  SharpenUpdate update;
  update.amount    = ReadOptionalFloat(object, {"amount", "offset"}, "sharpen");
  update.radius    = ReadOptionalFloat(object, {"radius"}, "sharpen");
  update.threshold = ReadOptionalFloat(object, {"threshold"}, "sharpen");
  return update;
}

auto ParseCat02Update(const nlohmann::json& params) -> Cat02WhiteBalanceUpdate {
  const auto& object =
      (params.contains("grade_white_balance") && params.at("grade_white_balance").is_object())
          ? UnwrapObject(params, {"grade_white_balance"}, "grade_white_balance")
          : RequireObject(params, "grade_white_balance");
  RejectUnknownKeys(object, {"enabled", "temperature", "tint"}, "grade_white_balance");
  Cat02WhiteBalanceUpdate update;
  update.enabled     = ReadOptionalBool(object, {"enabled"}, "grade_white_balance");
  update.temperature = ReadOptionalFloat(object, {"temperature"}, "grade_white_balance");
  update.tint        = ReadOptionalFloat(object, {"tint"}, "grade_white_balance");
  return update;
}

auto ParseDrtMethod(std::string_view value) -> DrtMethod {
  if (value == "aces_2_0") {
    return DrtMethod::Aces20;
  }
  if (value == "open_drt") {
    return DrtMethod::OpenDrt;
  }
  if (value == "film_print") {
    return DrtMethod::FilmPrint;
  }
  throw std::invalid_argument("Unknown odt.method: " + std::string{value});
}

auto ParseDrtColorSpace(std::string_view value, std::string_view key) -> DrtColorSpace {
  if (value == "rec709") {
    return DrtColorSpace::Rec709;
  }
  if (value == "rec2020") {
    return DrtColorSpace::Rec2020;
  }
  if (value == "p3_d65") {
    return DrtColorSpace::P3D65;
  }
  throw std::invalid_argument("Unknown odt." + std::string{key} + ": " + std::string{value});
}

auto ParseDrtEotf(std::string_view value) -> DrtEotf {
  if (value == "linear") return DrtEotf::Linear;
  if (value == "st2084") return DrtEotf::St2084;
  if (value == "hlg") return DrtEotf::Hlg;
  if (value == "gamma_2_6") return DrtEotf::Gamma26;
  if (value == "bt1886") return DrtEotf::Bt1886;
  if (value == "gamma_1_8") return DrtEotf::Gamma18;
  if (value == "srgb_piecewise") return DrtEotf::SrgbPiecewise;
  if (value == "gamma_2_2") return DrtEotf::Gamma22;
  throw std::invalid_argument("Unknown odt.encoding_eotf: " + std::string{value});
}

auto ParseDetailedDrtParameters(const nlohmann::json& value) -> OpenDrtDetailedParams {
  const auto& object = RequireObject(value, "odt.open_drt.parameters");
  RejectUnknownKeys(
      object,
      {"tn_con",      "tn_sh",      "tn_toe",    "tn_off",       "tn_hcon",     "tn_hcon_pv",
       "tn_hcon_st",  "tn_lcon",    "tn_lcon_w", "cwp_lm",       "rs_sa",       "rs_rw",
       "rs_bw",       "pt_lml",     "pt_lml_r",  "pt_lml_g",     "pt_lml_b",    "pt_lmh",
       "pt_lmh_r",    "pt_lmh_b",   "ptl_c",     "ptl_m",        "ptl_y",       "ptm_low",
       "ptm_low_rng", "ptm_low_st", "ptm_high",  "ptm_high_rng", "ptm_high_st", "brl",
       "brl_r",       "brl_g",      "brl_b",     "brl_rng",      "brl_st",      "brlp",
       "brlp_r",      "brlp_g",     "brlp_b",    "hc_r",         "hc_r_rng",    "hs_r",
       "hs_r_rng",    "hs_g",       "hs_g_rng",  "hs_b",         "hs_b_rng",    "hs_c",
       "hs_c_rng",    "hs_m",       "hs_m_rng",  "hs_y",         "hs_y_rng"},
      "odt.open_drt.parameters");
  OpenDrtDetailedParams result;
#define ALCEDO_READ_REQUIRED_ODRT(field)                                    \
  if (!object.contains(#field)) {                                           \
    throw std::invalid_argument("Missing odt.open_drt.parameters." #field); \
  }                                                                         \
  result.field = ReadFiniteFloat(object.at(#field), "odt.open_drt.parameters." #field)
  ALCEDO_READ_REQUIRED_ODRT(tn_con);
  ALCEDO_READ_REQUIRED_ODRT(tn_sh);
  ALCEDO_READ_REQUIRED_ODRT(tn_toe);
  ALCEDO_READ_REQUIRED_ODRT(tn_off);
  ALCEDO_READ_REQUIRED_ODRT(tn_hcon);
  ALCEDO_READ_REQUIRED_ODRT(tn_hcon_pv);
  ALCEDO_READ_REQUIRED_ODRT(tn_hcon_st);
  ALCEDO_READ_REQUIRED_ODRT(tn_lcon);
  ALCEDO_READ_REQUIRED_ODRT(tn_lcon_w);
  ALCEDO_READ_REQUIRED_ODRT(cwp_lm);
  ALCEDO_READ_REQUIRED_ODRT(rs_sa);
  ALCEDO_READ_REQUIRED_ODRT(rs_rw);
  ALCEDO_READ_REQUIRED_ODRT(rs_bw);
  ALCEDO_READ_REQUIRED_ODRT(pt_lml);
  ALCEDO_READ_REQUIRED_ODRT(pt_lml_r);
  ALCEDO_READ_REQUIRED_ODRT(pt_lml_g);
  ALCEDO_READ_REQUIRED_ODRT(pt_lml_b);
  ALCEDO_READ_REQUIRED_ODRT(pt_lmh);
  ALCEDO_READ_REQUIRED_ODRT(pt_lmh_r);
  ALCEDO_READ_REQUIRED_ODRT(pt_lmh_b);
  ALCEDO_READ_REQUIRED_ODRT(ptl_c);
  ALCEDO_READ_REQUIRED_ODRT(ptl_m);
  ALCEDO_READ_REQUIRED_ODRT(ptl_y);
  ALCEDO_READ_REQUIRED_ODRT(ptm_low);
  ALCEDO_READ_REQUIRED_ODRT(ptm_low_rng);
  ALCEDO_READ_REQUIRED_ODRT(ptm_low_st);
  ALCEDO_READ_REQUIRED_ODRT(ptm_high);
  ALCEDO_READ_REQUIRED_ODRT(ptm_high_rng);
  ALCEDO_READ_REQUIRED_ODRT(ptm_high_st);
  ALCEDO_READ_REQUIRED_ODRT(brl);
  ALCEDO_READ_REQUIRED_ODRT(brl_r);
  ALCEDO_READ_REQUIRED_ODRT(brl_g);
  ALCEDO_READ_REQUIRED_ODRT(brl_b);
  ALCEDO_READ_REQUIRED_ODRT(brl_rng);
  ALCEDO_READ_REQUIRED_ODRT(brl_st);
  ALCEDO_READ_REQUIRED_ODRT(brlp);
  ALCEDO_READ_REQUIRED_ODRT(brlp_r);
  ALCEDO_READ_REQUIRED_ODRT(brlp_g);
  ALCEDO_READ_REQUIRED_ODRT(brlp_b);
  ALCEDO_READ_REQUIRED_ODRT(hc_r);
  ALCEDO_READ_REQUIRED_ODRT(hc_r_rng);
  ALCEDO_READ_REQUIRED_ODRT(hs_r);
  ALCEDO_READ_REQUIRED_ODRT(hs_r_rng);
  ALCEDO_READ_REQUIRED_ODRT(hs_g);
  ALCEDO_READ_REQUIRED_ODRT(hs_g_rng);
  ALCEDO_READ_REQUIRED_ODRT(hs_b);
  ALCEDO_READ_REQUIRED_ODRT(hs_b_rng);
  ALCEDO_READ_REQUIRED_ODRT(hs_c);
  ALCEDO_READ_REQUIRED_ODRT(hs_c_rng);
  ALCEDO_READ_REQUIRED_ODRT(hs_m);
  ALCEDO_READ_REQUIRED_ODRT(hs_m_rng);
  ALCEDO_READ_REQUIRED_ODRT(hs_y);
  ALCEDO_READ_REQUIRED_ODRT(hs_y_rng);
#undef ALCEDO_READ_REQUIRED_ODRT
  return result;
}

/**
 * Fills each film print field that @p update does not set as the Model loads it
 * (`DrtParamsModel::LoadJson`): with no `film_print` object (@p has_film_print false), the defaults
 * of new settings; in a `film_print` object, the defaults, and the stored defaults
 * (`kFilmScanAutoStoredDefault`) for `auto` and `auto_strength`.
 */
void CompleteFilmPrintUpdate(DrtParameterUpdate& update, bool has_film_print) {
  const FilmPrintParams defaults;
  const auto&           scan = defaults.scan;
  const auto            fill = [](auto& field, const auto& value) {
    if (!field.has_value()) field = value;
  };
  fill(update.film_negative, defaults.negative);
  fill(update.film_print_material, defaults.print_material);
  fill(update.film_scene_light, defaults.scene_light);
  fill(update.film_light_spread, defaults.light_spread);
  fill(update.film_halation, defaults.halation);
  fill(update.film_enlarger_flare, defaults.enlarger_flare);
  fill(update.film_scan_auto,
       has_film_print ? std::string{kFilmScanAutoStoredDefault} : scan.auto_colour);
  fill(update.film_scan_auto_strength,
       has_film_print ? kFilmScanAutoStrengthStoredDefault : scan.auto_strength);
  fill(update.film_scan_auto_density, scan.auto_density);
  fill(update.film_scan_contrast, scan.contrast);
  fill(update.film_scan_exposure_stops, scan.exposure_stops);
  fill(update.film_scan_cyan, scan.cyan);
  fill(update.film_scan_magenta, scan.magenta);
  fill(update.film_scan_yellow, scan.yellow);
  fill(update.film_scan_black, scan.black);
  fill(update.film_scanner_red_nm, scan.scanner_red_nm);
  fill(update.film_scanner_green_nm, scan.scanner_green_nm);
  fill(update.film_scanner_blue_nm, scan.scanner_blue_nm);
  fill(update.film_scanner_width_nm, scan.scanner_width_nm);
  fill(update.film_scanner_light, scan.scanner_light);
}

auto ParseDrtUpdate(const nlohmann::json& params) -> DrtParameterUpdate {
  const auto& object = UnwrapObject(params, {"odt"}, "odt");
  RejectUnknownKeys(object,
                    {"method", "encoding_space", "encoding_eotf", "limiting_space",
                     "peak_luminance", "open_drt", "film_print"},
                    "odt");
  DrtParameterUpdate update;
  if (const auto value = ReadOptionalString(object, {"method"}, "odt"); value.has_value()) {
    update.method = ParseDrtMethod(*value);
  }
  if (const auto value = ReadOptionalString(object, {"encoding_space"}, "odt"); value.has_value()) {
    update.encoding_space = ParseDrtColorSpace(*value, "encoding_space");
  }
  if (const auto value = ReadOptionalString(object, {"encoding_eotf"}, "odt"); value.has_value()) {
    update.encoding_eotf = ParseDrtEotf(*value);
  }
  if (const auto value = ReadOptionalString(object, {"limiting_space"}, "odt"); value.has_value()) {
    update.limiting_space = ParseDrtColorSpace(*value, "limiting_space");
  }
  update.peak_luminance = ReadOptionalFloat(object, {"peak_luminance"}, "odt");
  if (object.contains("open_drt")) {
    const auto& open_drt = RequireObject(object.at("open_drt"), "odt.open_drt");
    RejectUnknownKeys(open_drt,
                      {"look_preset", "tonescale_preset", "creative_white", "creative_white_limit",
                       "display_grey_luminance", "hdr_grey_boost", "hdr_purity", "parameters"},
                      "odt.open_drt");
    update.look_preset      = ReadOptionalString(open_drt, {"look_preset"}, "odt.open_drt");
    update.tonescale_preset = ReadOptionalString(open_drt, {"tonescale_preset"}, "odt.open_drt");
    update.creative_white   = ReadOptionalString(open_drt, {"creative_white"}, "odt.open_drt");
    update.creative_white_limit =
        ReadOptionalFloat(open_drt, {"creative_white_limit"}, "odt.open_drt");
    update.display_grey_luminance =
        ReadOptionalFloat(open_drt, {"display_grey_luminance"}, "odt.open_drt");
    update.hdr_grey_boost = ReadOptionalFloat(open_drt, {"hdr_grey_boost"}, "odt.open_drt");
    update.hdr_purity     = ReadOptionalFloat(open_drt, {"hdr_purity"}, "odt.open_drt");
    if (open_drt.contains("parameters")) {
      update.parameters = ParseDetailedDrtParameters(open_drt.at("parameters"));
    }
  }
  if (object.contains("film_print")) {
    const auto& film_print = RequireObject(object.at("film_print"), "odt.film_print");
    RejectUnknownKeys(film_print,
                      {"negative", "print_material", "scene_light", "light_spread", "halation",
                       "enlarger_flare", "auto", "auto_strength", "auto_density", "scan_contrast",
                       "scan_exposure_stops", "cyan", "magenta", "yellow", "black",
                       "scanner_red_nm", "scanner_green_nm", "scanner_blue_nm",
                       "scanner_width_nm", "scanner_light"},
                      "odt.film_print");
    update.film_negative = ReadOptionalString(film_print, {"negative"}, "odt.film_print");
    update.film_print_material =
        ReadOptionalString(film_print, {"print_material"}, "odt.film_print");
    update.film_scene_light  = ReadOptionalString(film_print, {"scene_light"}, "odt.film_print");
    update.film_light_spread = ReadOptionalFloat(film_print, {"light_spread"}, "odt.film_print");
    update.film_halation     = ReadOptionalFloat(film_print, {"halation"}, "odt.film_print");
    update.film_enlarger_flare =
        ReadOptionalFloat(film_print, {"enlarger_flare"}, "odt.film_print");
    const auto number = [&film_print](const char* key) {
      return ReadOptionalFloat(film_print, {key}, "odt.film_print");
    };
    update.film_scan_auto           = ReadOptionalString(film_print, {"auto"}, "odt.film_print");
    update.film_scan_auto_strength  = number("auto_strength");
    update.film_scan_auto_density   = number("auto_density");
    update.film_scan_contrast       = number("scan_contrast");
    update.film_scan_exposure_stops = number("scan_exposure_stops");
    update.film_scan_cyan           = number("cyan");
    update.film_scan_magenta        = number("magenta");
    update.film_scan_yellow         = number("yellow");
    update.film_scan_black          = number("black");
    update.film_scanner_red_nm      = number("scanner_red_nm");
    update.film_scanner_green_nm    = number("scanner_green_nm");
    update.film_scanner_blue_nm     = number("scanner_blue_nm");
    update.film_scanner_width_nm    = number("scanner_width_nm");
    update.film_scanner_light = ReadOptionalString(film_print, {"scanner_light"}, "odt.film_print");
  }
  return update;
}

/// `camera_profile_file` (dcp_camera_profile_plan.md, phase C4).
///
/// A write is a selection: {"camera_model", "profile_name"} with an optional boolean
/// "profile_tone" (default true; dcp_profile_tone_curve_study.md, section 6.3), or the same object
/// under "camera_profile_file". `{}`, null, and {"camera_profile_file": null} clear the reference.
/// The identity is resolved through the DCP library; an identity that the library does not hold is
/// rejected, so the document stays unchanged.
///
/// A value is a stored Develop JSON, complete or reduced to the owned key: the reference with its
/// fingerprint under "camera_profile_file", and no key for no reference. The other keys belong to
/// other Develop fields. An identity that the library does not hold gives an unbound reference,
/// so a project opens without its DCP folder and its render fails with the named error.
auto ParseCameraProfileFileUpdate(const nlohmann::json& params, ParseForm form)
    -> DevelopCameraProfileFileUpdate {
  constexpr std::string_view kField = "camera_profile_file";
  RequireObject(params, kField);
  const nlohmann::json* reference = &params;
  if (params.contains(kField)) {
    reference = &params.at(std::string{kField});
    if (form == ParseForm::Write && params.size() != 1) {
      RejectUnknownKeys(params, {kField}, kField);
    }
  } else if (form == ParseForm::Value && !params.contains("camera_model")) {
    return {};
  }
  if (reference->is_null() || (reference->is_object() && reference->empty())) {
    return {};
  }
  if (form == ParseForm::Value) {
    const auto                     stored   = DevelopCameraProfileFileFromJson(*reference);
    auto                           resolved = ResolveStoredDcpCameraProfileFile(stored);
    DevelopCameraProfileFileUpdate update;
    update.profile_tone = resolved.profile_tone;
    if (resolved.IsBound()) {
      update.profile = std::move(resolved.profile);
    } else {
      update.unresolved = std::move(resolved);
    }
    return update;
  }
  RequireObject(*reference, kField);
  RejectUnknownKeys(*reference, {"camera_model", "profile_name", "profile_tone"}, kField);
  const auto camera_model = ReadOptionalString(*reference, {"camera_model"}, kField);
  const auto profile_name = ReadOptionalString(*reference, {"profile_name"}, kField);
  if (!camera_model.has_value() || camera_model->empty() || !profile_name.has_value() ||
      profile_name->empty()) {
    throw std::invalid_argument(
        "camera_profile_file: a selection needs the strings camera_model and profile_name");
  }
  DevelopCameraProfileFileUpdate update;
  update.profile      = FindSelectableDcpCameraProfile(*camera_model, *profile_name);
  update.profile_tone = ReadOptionalBool(*reference, {"profile_tone"}, kField).value_or(true);
  return update;
}

auto ParseWriteOrThrow(std::string_view field, const nlohmann::json& params, ParseForm form)
    -> EditorParameterWrite {
  if (field == "exposure") {
    return ParseScalarWrite(params, field, "exposure_ev");
  }
  if (field == "contrast") {
    return ParseScalarWrite(params, field, "contrast");
  }
  if (field == "white" || field == "whites") {
    return ParseScalarWrite(params, field, "white");
  }
  if (field == "black" || field == "blacks") {
    return ParseScalarWrite(params, field, "black");
  }
  if (field == "shadows") {
    return ParseScalarWrite(params, field, "shadows");
  }
  if (field == "highlights") {
    return ParseScalarWrite(params, field, "highlights");
  }
  if (field == "saturation") {
    return ParseScalarWrite(params, field, "saturation");
  }
  if (field == "vibrance") {
    return ParseScalarWrite(params, field, "vibrance");
  }
  if (field == "clarity") {
    return ParseScalarWrite(params, field, "clarity");
  }
  if (field == "halation") {
    return ParseScalarWrite(params, field, "strength");
  }
  if (field == "film_grain") {
    return ParseScalarWrite(params, field, "strength");
  }
  if (field == "diffusion") {
    return ParseScalarWrite(params, field, "strength");
  }
  if (field == "film_format") {
    return ParseFilmFormatUpdate(params);
  }
  if (field == "curve") {
    return ParseCurveWrite(params, form);
  }
  if (field == "lut" || field == "ocio_lmt") {
    return ParseLutWrite(params);
  }
  if (field == "hls" || field == "HLS") {
    return ParseHlsUpdate(params);
  }
  if (field == "color_wheel") {
    return ParseColorWheelUpdate(params);
  }
  if (field == "grade_white_balance") {
    return ParseCat02Update(params);
  }
  if (field == "sharpen") {
    return ParseSharpenUpdate(params);
  }
  if (field == "raw_decode") {
    return ParseRawDecodeUpdate(params);
  }
  if (field == "color_temp") {
    return ParseColorTemperatureUpdate(params);
  }
  if (field == "lens_calib") {
    return ParseLensCalibrationUpdate(params);
  }
  if (field == "input_profile") {
    return ParseInputProfileUpdate(params);
  }
  if (field == "camera_profile_file") {
    return ParseCameraProfileFileUpdate(params, form);
  }
  if (field == "odt") {
    auto update = ParseDrtUpdate(params);
    // The Model omits a film print key at its default (and the film print object of another
    // method), so a complete value reads such a key as the default (issue 661).
    if (form == ParseForm::Value) {
      CompleteFilmPrintUpdate(update, UnwrapObject(params, {"odt"}, "odt").contains("film_print"));
    }
    return update;
  }
  if (field == "crop_rotate") {
    return ParseGeometryUpdate(params);
  }
  RequireObject(params, field);
  if (params.contains("index") && params.contains("value") && params.at("value").is_string()) {
    return EditorEnumWrite{params.at("value").get<std::string>()};
  }
  if (params.contains("value") && params.at("value").is_boolean() && params.size() == 1) {
    return EditorToggleWrite{params.at("value").get<bool>()};
  }
  throw std::invalid_argument("Unsupported editor parameter field: " + std::string{field});
}

}  // namespace

auto ParseEditorParameterWrite(std::string_view field_key, const nlohmann::json& params,
                               std::string* error) -> std::optional<EditorParameterWrite> {
  const nlohmann::json& object = params.is_null() ? nlohmann::json::object() : params;
  if (!object.is_object()) {
    SetError(error, "Editor parameter params must be a JSON object");
    return std::nullopt;
  }
  try {
    return ParseWriteOrThrow(field_key, object, ParseForm::Write);
  } catch (const std::exception& ex) {
    SetError(error, ex.what());
    return std::nullopt;
  }
}

auto ParseEditorParameterValue(std::string_view field_key, const nlohmann::json& value,
                               std::string* error) -> std::optional<EditorParameterWrite> {
  const nlohmann::json& object = value.is_null() ? nlohmann::json::object() : value;
  if (!object.is_object()) {
    SetError(error, "Editor parameter value must be a JSON object");
    return std::nullopt;
  }
  try {
    return ParseWriteOrThrow(field_key, object, ParseForm::Value);
  } catch (const std::exception& ex) {
    SetError(error, ex.what());
    return std::nullopt;
  }
}

}  // namespace alcedo
