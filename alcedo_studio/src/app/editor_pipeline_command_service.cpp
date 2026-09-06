//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_pipeline_command_service.hpp"

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

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/graph_validation.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/cat02_white_balance_model.hpp"
#include "edit/operators/models/color_wheel_model.hpp"
#include "edit/operators/models/curve_model.hpp"
#include "edit/operators/models/hls_model.hpp"
#include "edit/operators/models/lmt_model.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/operators/models/sharpen_model.hpp"

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

template <typename Model>
void ApplyScalarPatch(IOperatorModel& base, const nlohmann::json& params, std::string_view field,
                      std::string_view canonical_key) {
  auto* model = dynamic_cast<Model*>(&base);
  if (model == nullptr) {
    throw std::invalid_argument("Adjustment Model type does not match field " + std::string{field});
  }
  const auto& object = (params.contains(field) && params.at(std::string{field}).is_object())
                           ? UnwrapObject(params, {field}, field)
                           : RequireObject(params, field);
  RejectUnknownKeys(object, {canonical_key, field, "value"}, field);
  const auto value = ReadOptionalFloat(object, {canonical_key, field, "value"}, field);
  if (value.has_value()) {
    model->SetValue(*value);
  }
}

auto ParseCurvePoints(const nlohmann::json& params) -> std::optional<std::vector<CurvePoint>> {
  const auto& object = UnwrapObject(params, {"curve"}, "curve");
  RejectUnknownKeys(object, {"points", "size"}, "curve");
  if (!object.contains("points")) {
    return std::nullopt;
  }
  const auto& points_json = object.at("points");
  if (!points_json.is_array() || points_json.size() < 2) {
    throw std::invalid_argument("curve.points must contain at least two points");
  }
  if (object.contains("size")) {
    if (!object.at("size").is_number_integer() ||
        object.at("size").get<std::size_t>() != points_json.size()) {
      throw std::invalid_argument("curve.size must match curve.points");
    }
  }
  std::vector<CurvePoint> points;
  points.reserve(points_json.size());
  for (std::size_t index = 0; index < points_json.size(); ++index) {
    const auto& point =
        RequireObject(points_json.at(index), "curve.points[" + std::to_string(index) + "]");
    RejectUnknownKeys(point, {"x", "y"}, "curve point");
    if (!point.contains("x") || !point.contains("y")) {
      throw std::invalid_argument("curve points require x and y");
    }
    points.push_back(CurvePoint{ReadFiniteFloat(point.at("x"), "curve point.x"),
                                ReadFiniteFloat(point.at("y"), "curve point.y")});
  }
  return points;
}

auto ParseLmtPath(const nlohmann::json& params) -> std::optional<std::string> {
  RequireObject(params, "lut");
  const auto& object = (params.contains("lut") && params.at("lut").is_object())
                           ? UnwrapObject(params, {"lut"}, "lut")
                       : (params.contains("ocio_lmt") && params.at("ocio_lmt").is_object())
                           ? UnwrapObject(params, {"ocio_lmt"}, "lut")
                           : params;
  RejectUnknownKeys(object, {"cube_path", "ocio_lmt", "lut", "value"}, "lut");
  return ReadOptionalString(object, {"cube_path", "ocio_lmt", "lut", "value"}, "lut");
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
  RejectUnknownKeys(object, {"lift", "gamma", "gain"}, "color_wheel");
  ColorWheelUpdate update;
  if (object.contains("lift")) {
    update.lift = ParseColorWheelControl(object.at("lift"), "color_wheel.lift");
  }
  if (object.contains("gamma")) {
    update.gamma = ParseColorWheelControl(object.at("gamma"), "color_wheel.gamma");
  }
  if (object.contains("gain")) {
    update.gain = ParseColorWheelControl(object.at("gain"), "color_wheel.gain");
  }
  return update;
}

auto ParseRawDecodeUpdate(const nlohmann::json& params) -> DevelopRawDecodeUpdate {
  const auto& object = UnwrapObject(params, {"raw", "raw_decode"}, "raw_decode");
  // History reads the complete Develop Model JSON. Known non-RAW fields are accepted as
  // owner-local persistence context and are intentionally not copied into this update.
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
                     "lens_profile_db_path"},
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
  // History reads the complete Develop Model JSON. Only white-balance fields are applied.
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
                     "lens_profile_db_path"},
                    "color_temp");
  DevelopColorTemperatureUpdate update;
  update.wb_mode      = ReadOptionalString(object, {"wb_mode", "mode"}, "color_temp");
  update.custom_cct   = ReadOptionalFloat(object, {"custom_cct", "cct"}, "color_temp");
  update.custom_tint  = ReadOptionalFloat(object, {"custom_tint", "tint"}, "color_temp");
  update.as_shot_cct  = ReadOptionalFloat(object, {"as_shot_cct"}, "color_temp");
  update.as_shot_tint = ReadOptionalFloat(object, {"as_shot_tint"}, "color_temp");
  return update;
}

auto ParseLensCalibrationUpdate(const nlohmann::json& params) -> DevelopLensCalibrationUpdate {
  const auto& object = UnwrapObject(params, {"lens_calib"}, "lens_calib");
  // Camera metadata and other Develop fields are persistence context, not lens update inputs.
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
                     "camera_profile"},
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

auto ParseGeometryUpdate(const nlohmann::json& params) -> ImageGeometryUpdate {
  const auto& object = UnwrapObject(params, {"crop_rotate"}, "crop_rotate");
  RejectUnknownKeys(object,
                    {"crop_rect", "rotation_degrees", "angle_degrees", "expand_to_fit", "enabled",
                     "enable_crop", "aspect_ratio_preset", "aspect_ratio", "source_size"},
                    "crop_rotate");
  ImageGeometryUpdate update;
  if (object.contains("crop_rect")) {
    update.crop_rect = ParseNormalizedRect(object.at("crop_rect"), "crop_rotate.crop_rect");
  }
  update.rotation_degrees =
      ReadOptionalFloat(object, {"rotation_degrees", "angle_degrees"}, "crop_rotate");
  update.expand_to_fit = ReadOptionalBool(object, {"expand_to_fit"}, "crop_rotate");
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
      (params.contains("cat02_white_balance") && params.at("cat02_white_balance").is_object())
          ? UnwrapObject(params, {"cat02_white_balance"}, "tint")
          : RequireObject(params, "tint");
  RejectUnknownKeys(object, {"enabled", "temperature_offset", "tint_offset", "value", "tint"},
                    "tint");
  Cat02WhiteBalanceUpdate update;
  update.enabled            = ReadOptionalBool(object, {"enabled"}, "tint");
  update.temperature_offset = ReadOptionalFloat(object, {"temperature_offset"}, "tint");
  update.tint_offset        = ReadOptionalFloat(object, {"tint_offset", "value", "tint"}, "tint");
  return update;
}

auto ParseDrtMethod(std::string_view value) -> DrtMethod {
  if (value == "aces_2_0") {
    return DrtMethod::Aces20;
  }
  if (value == "open_drt") {
    return DrtMethod::OpenDrt;
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

auto ParseDrtUpdate(const nlohmann::json& params) -> DrtParameterUpdate {
  const auto& object = UnwrapObject(params, {"odt"}, "odt");
  RejectUnknownKeys(
      object,
      {"method", "encoding_space", "encoding_eotf", "limiting_space", "peak_luminance", "open_drt"},
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
  return update;
}

auto JoinValidation(const std::vector<GraphValidationError>& errors) -> std::string {
  std::string text;
  for (const auto& item : errors) {
    if (!text.empty()) {
      text += "; ";
    }
    text += item.message;
  }
  return text;
}

auto ColorGradeOf(PipelineDocument& document, const NodeId& id, std::string* error)
    -> ColorGradeNodeModel* {
  auto* grade = dynamic_cast<ColorGradeNodeModel*>(document.Graph().FindNode(id));
  if (grade == nullptr) {
    SetError(error, "Color Grade node is missing: " + std::string(id.Value()));
    return nullptr;
  }
  return grade;
}

auto ColorGradeOf(const PipelineDocument& document, const NodeId& id, std::string* error)
    -> const ColorGradeNodeModel* {
  const auto* grade = dynamic_cast<const ColorGradeNodeModel*>(document.Graph().FindNode(id));
  if (grade == nullptr) {
    SetError(error, "Color Grade node is missing: " + std::string(id.Value()));
    return nullptr;
  }
  return grade;
}

auto IsDrtPostAdjustmentField(std::string_view field) -> bool {
  return field == "clarity" || field == "sharpen" || field == "halation" || field == "film_grain";
}

auto OperatorTypeForCurrentPanelField(std::string_view field) -> const OperatorTypeId* {
  if (field == "exposure") return &type_ids::Exposure();
  if (field == "contrast") return &type_ids::Contrast();
  if (field == "white" || field == "whites") return &type_ids::White();
  if (field == "black" || field == "blacks") return &type_ids::Black();
  if (field == "shadows") return &type_ids::Shadows();
  if (field == "highlights") return &type_ids::Highlights();
  if (field == "curve") return &type_ids::Curve();
  if (field == "saturation") return &type_ids::Saturation();
  if (field == "vibrance") return &type_ids::Vibrance();
  if (field == "tint") return &type_ids::Cat02WhiteBalance();
  if (field == "hls" || field == "HLS") return &type_ids::Hls();
  if (field == "color_wheel") return &type_ids::ColorWheel();
  if (field == "lut" || field == "ocio_lmt") return &type_ids::Lmt();
  if (field == "clarity") return &type_ids::Clarity();
  if (field == "sharpen") return &type_ids::Sharpen();
  if (field == "halation") return &type_ids::Halation();
  if (field == "film_grain") return &type_ids::FilmGrain();
  return nullptr;
}

auto CurrentPanelColorGrade(const PipelineDocument& document) -> const ColorGradeNodeModel* {
  if (const auto* primary = document.PrimaryGrade(); primary != nullptr) {
    return primary;
  }
  const auto grades = ColorGradesOnImageBackbone(document);
  return grades.empty() ? nullptr : grades.front();
}

auto DrtPostModel(DrtNodeModel& drt, const AdjustmentInstanceId& id, std::string* error)
    -> IOperatorModel* {
  auto* model = drt.FindAdjustment(id);
  if (model == nullptr) {
    SetError(error, "Adjustment instance is missing: " + std::string(id.Value()));
  }
  return model;
}

auto DrtPostModel(const DrtNodeModel& drt, const AdjustmentInstanceId& id, std::string* error)
    -> const IOperatorModel* {
  const auto* model = drt.FindAdjustment(id);
  if (model == nullptr) {
    SetError(error, "Adjustment instance is missing: " + std::string(id.Value()));
  }
  return model;
}

void ApplyColorGradeModelPatch(IOperatorModel& model, std::string_view field,
                               const nlohmann::json& params) {
  if (field == "exposure") {
    ApplyScalarPatch<ExposureModel>(model, params, field, "exposure_ev");
    return;
  }
  if (field == "contrast") {
    ApplyScalarPatch<ContrastModel>(model, params, field, "contrast");
    return;
  }
  if (field == "white" || field == "whites") {
    ApplyScalarPatch<WhiteModel>(model, params, field, "white");
    return;
  }
  if (field == "black" || field == "blacks") {
    ApplyScalarPatch<BlackModel>(model, params, field, "black");
    return;
  }
  if (field == "shadows") {
    ApplyScalarPatch<ShadowsModel>(model, params, field, "shadows");
    return;
  }
  if (field == "highlights") {
    ApplyScalarPatch<HighlightsModel>(model, params, field, "highlights");
    return;
  }
  if (field == "saturation") {
    ApplyScalarPatch<SaturationModel>(model, params, field, "saturation");
    return;
  }
  if (field == "vibrance") {
    ApplyScalarPatch<VibranceModel>(model, params, field, "vibrance");
    return;
  }
  if (field == "curve") {
    const auto points = ParseCurvePoints(params);
    if (!points.has_value()) {
      return;
    }
    auto* typed = dynamic_cast<CurveModel*>(&model);
    if (typed == nullptr) {
      throw std::invalid_argument("Adjustment Model type does not match field curve");
    }
    typed->SetPoints(*points);
    return;
  }
  if (field == "tint") {
    auto  update = ParseCat02Update(params);
    auto* typed  = dynamic_cast<Cat02WhiteBalanceModel*>(&model);
    if (typed == nullptr) {
      throw std::invalid_argument("Adjustment Model type does not match field tint");
    }
    typed->ApplyUpdate(std::move(update));
    return;
  }
  if (field == "hls" || field == "HLS") {
    auto  update = ParseHlsUpdate(params);
    auto* typed  = dynamic_cast<HlsModel*>(&model);
    if (typed == nullptr) {
      throw std::invalid_argument("Adjustment Model type does not match field HLS");
    }
    typed->ApplyUpdate(std::move(update));
    return;
  }
  if (field == "color_wheel") {
    auto  update = ParseColorWheelUpdate(params);
    auto* typed  = dynamic_cast<ColorWheelModel*>(&model);
    if (typed == nullptr) {
      throw std::invalid_argument("Adjustment Model type does not match field color_wheel");
    }
    typed->ApplyUpdate(std::move(update));
    return;
  }
  if (field == "lut" || field == "ocio_lmt") {
    const auto path  = ParseLmtPath(params);
    auto*      typed = dynamic_cast<LmtModel*>(&model);
    if (typed == nullptr) {
      throw std::invalid_argument("Adjustment Model type does not match field lut");
    }
    if (path.has_value()) {
      typed->SetCubePath(*path);
    }
    return;
  }
  throw std::invalid_argument("Unsupported Color Grade parameter field: " + std::string{field});
}

void ApplyDrtPostAdjustmentPatch(IOperatorModel& model, std::string_view field,
                                 const nlohmann::json& params) {
  if (field == "clarity") {
    ApplyScalarPatch<ClarityModel>(model, params, field, "clarity");
    return;
  }
  if (field == "halation") {
    ApplyScalarPatch<HalationModel>(model, params, field, "strength");
    return;
  }
  if (field == "film_grain") {
    ApplyScalarPatch<FilmGrainModel>(model, params, field, "strength");
    return;
  }
  if (field == "sharpen") {
    auto  update = ParseSharpenUpdate(params);
    auto* typed  = dynamic_cast<SharpenModel*>(&model);
    if (typed == nullptr) {
      throw std::invalid_argument("Adjustment Model type does not match field sharpen");
    }
    typed->ApplyUpdate(std::move(update));
    return;
  }
  throw std::invalid_argument("Unsupported DRT/Post parameter field: " + std::string{field});
}

}  // namespace

auto ApplyEditorParameterPatch(PipelineDocument& document, const EditorParameterTarget& target,
                               const nlohmann::json& params, std::string* error) -> bool {
  const auto target_error = DescribeEditorParameterTargetError(target, target.field_key);
  if (!target_error.empty()) {
    return SetError(error, target_error);
  }
  if (!params.is_object() && !params.is_null()) {
    return SetError(error, "Editor parameter params must be a JSON object");
  }
  const nlohmann::json patch = params.is_null() ? nlohmann::json::object() : params;

  try {
    switch (target.owner_kind) {
      case EditorParameterOwnerKind::ColorGrade: {
        auto* grade = ColorGradeOf(document, target.node_id, error);
        if (grade == nullptr) {
          return false;
        }
        auto* model = grade->FindAdjustment(target.adjustment_instance_id);
        if (model == nullptr) {
          return SetError(error, "Adjustment instance is missing: " +
                                     std::string(target.adjustment_instance_id.Value()));
        }
        ApplyColorGradeModelPatch(*model, target.field_key, patch);
        return true;
      }
      case EditorParameterOwnerKind::Document: {
        auto update = ParseGeometryUpdate(patch);
        document.Geometry().ApplyUpdate(update);
        return true;
      }
      case EditorParameterOwnerKind::Develop: {
        auto* develop = document.Develop();
        if (develop == nullptr || develop->Id() != target.node_id) {
          return SetError(error, "Develop node is missing: " + std::string(target.node_id.Value()));
        }
        if (target.field_key == "raw_decode") {
          auto update = ParseRawDecodeUpdate(patch);
          develop->Params().ApplyRawDecodeUpdate(std::move(update));
        } else if (target.field_key == "color_temp") {
          auto update = ParseColorTemperatureUpdate(patch);
          develop->Params().ApplyColorTemperatureUpdate(std::move(update));
        } else if (target.field_key == "lens_calib") {
          auto update = ParseLensCalibrationUpdate(patch);
          develop->Params().ApplyLensCalibrationUpdate(std::move(update));
        } else {
          return SetError(error, "Unsupported Develop parameter field: " + target.field_key);
        }
        return true;
      }
      case EditorParameterOwnerKind::DrtPost: {
        auto* drt = document.Drt();
        if (drt == nullptr || drt->Id() != target.node_id) {
          return SetError(error, "DRT node is missing: " + std::string(target.node_id.Value()));
        }
        if (IsDrtPostAdjustmentField(target.field_key)) {
          auto* model = DrtPostModel(*drt, target.adjustment_instance_id, error);
          if (model == nullptr) {
            return false;
          }
          ApplyDrtPostAdjustmentPatch(*model, target.field_key, patch);
          return true;
        }
        if (target.field_key != "odt") {
          return SetError(error, "Unsupported DRT parameter field: " + target.field_key);
        }
        auto update = ParseDrtUpdate(patch);
        drt->Params().ApplyUpdate(std::move(update));
        return true;
      }
      default:
        return SetError(error, "Editor parameter target owner_kind is not supported");
    }
  } catch (const std::exception& ex) {
    return SetError(error, ex.what());
  }
}

auto ReadEditorParameterJson(const PipelineDocument& document, const EditorParameterTarget& target,
                             nlohmann::json* json, std::string* error) -> bool {
  if (json == nullptr) {
    return SetError(error, "Editor parameter JSON storage is required");
  }
  const auto target_error = DescribeEditorParameterTargetError(target, target.field_key);
  if (!target_error.empty()) {
    return SetError(error, target_error);
  }
  try {
    switch (target.owner_kind) {
      case EditorParameterOwnerKind::ColorGrade: {
        const auto* grade = ColorGradeOf(document, target.node_id, error);
        if (grade == nullptr) {
          return false;
        }
        const auto* model = grade->FindAdjustment(target.adjustment_instance_id);
        if (model == nullptr) {
          return SetError(error, "Adjustment instance is missing: " +
                                     std::string(target.adjustment_instance_id.Value()));
        }
        *json = model->ToJson();
        return true;
      }
      case EditorParameterOwnerKind::Document:
        *json = document.Geometry().ToJson();
        return true;
      case EditorParameterOwnerKind::Develop: {
        const auto* develop = document.Develop();
        if (develop == nullptr || develop->Id() != target.node_id) {
          return SetError(error, "Develop node is missing: " + std::string(target.node_id.Value()));
        }
        *json = develop->Params().ToJson();
        return true;
      }
      case EditorParameterOwnerKind::DrtPost: {
        const auto* drt = document.Drt();
        if (drt == nullptr || drt->Id() != target.node_id) {
          return SetError(error, "DRT node is missing: " + std::string(target.node_id.Value()));
        }
        if (IsDrtPostAdjustmentField(target.field_key)) {
          const auto* model = DrtPostModel(*drt, target.adjustment_instance_id, error);
          if (model == nullptr) {
            return false;
          }
          *json = model->ToJson();
          return true;
        }
        *json = drt->Params().ToJson();
        return true;
      }
      default:
        return SetError(error, "Editor parameter target owner_kind is not supported");
    }
  } catch (const std::exception& ex) {
    return SetError(error, ex.what());
  }
}

auto CanonicalPipelineDocumentJson(const PipelineDocument& document) -> std::string {
  return document.ToJson().dump();
}

auto PipelineDocumentPassesValidation(const PipelineDocument& document, std::string* error)
    -> bool {
  auto       errors   = document.Graph().Validate();
  const auto backbone = document.Graph().ValidateImageBackbone();
  errors.insert(errors.end(), backbone.begin(), backbone.end());
  if (errors.empty()) {
    return true;
  }
  return SetError(error, JoinValidation(errors));
}

auto PublishEditorParameterPatch(PipelineDocument& live, const EditorParameterTarget& target,
                                 const nlohmann::json& params, std::string* error) -> bool {
  return ApplyEditorParameterPatch(live, target, params, error);
}

auto CompleteCurrentPanelParameterTarget(const PipelineDocument& document, std::string field_key,
                                         std::string* error)
    -> std::optional<EditorParameterTarget> {
  EditorParameterTarget target;
  target.field_key = std::move(field_key);
  if (target.field_key == "crop_rotate") {
    target.owner_kind = EditorParameterOwnerKind::Document;
    return target;
  }
  if (target.field_key == "raw_decode" || target.field_key == "lens_calib" ||
      target.field_key == "color_temp") {
    const auto* develop = document.Develop();
    if (develop == nullptr) {
      SetError(error, "Develop node is missing");
      return std::nullopt;
    }
    target.owner_kind = EditorParameterOwnerKind::Develop;
    target.node_id    = develop->Id();
    return target;
  }
  if (target.field_key == "odt") {
    const auto* drt = document.Drt();
    if (drt == nullptr) {
      SetError(error, "DRT node is missing");
      return std::nullopt;
    }
    target.owner_kind = EditorParameterOwnerKind::DrtPost;
    target.node_id    = drt->Id();
    return target;
  }
  const auto* type = OperatorTypeForCurrentPanelField(target.field_key);
  if (type == nullptr) {
    SetError(error, "Unknown editor adjustment field: " + target.field_key);
    return std::nullopt;
  }
  if (IsDrtPostAdjustmentField(target.field_key)) {
    const auto* drt = document.Drt();
    if (drt == nullptr) {
      SetError(error, "DRT node is missing");
      return std::nullopt;
    }
    const auto* instance = drt->FindAdjustmentIdByType(*type);
    if (instance == nullptr) {
      SetError(error, "DRT/Post adjustment is missing: " + std::string{type->Text()});
      return std::nullopt;
    }
    target.owner_kind             = EditorParameterOwnerKind::DrtPost;
    target.node_id                = drt->Id();
    target.adjustment_instance_id = *instance;
    return target;
  }
  const auto* grade = CurrentPanelColorGrade(document);
  if (grade == nullptr) {
    SetError(error, "Color Grade node is missing");
    return std::nullopt;
  }
  const auto* instance = grade->FindAdjustmentIdByType(*type);
  if (instance == nullptr) {
    SetError(error, "Color Grade adjustment is missing: " + std::string{type->Text()});
    return std::nullopt;
  }
  target.owner_kind             = EditorParameterOwnerKind::ColorGrade;
  target.node_id                = grade->Id();
  target.adjustment_instance_id = *instance;
  return target;
}

}  // namespace alcedo
