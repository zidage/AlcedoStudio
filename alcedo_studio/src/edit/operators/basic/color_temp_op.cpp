//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/basic/color_temp_op.hpp"

#include <opencv2/core.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>

#include "edit/graph/develop_color_transform.hpp"

namespace alcedo {
namespace {
constexpr double kCustomCCTMin      = 2000.0;
constexpr double kCustomCCTMax      = 15000.0;
constexpr double kCustomTintMin     = -150.0;
constexpr double kCustomTintMax     = 150.0;
constexpr double kValueEpsilon      = 1e-10;

auto ClampFinite(double value, double min_value, double max_value) -> double {
  if (!std::isfinite(value)) {
    return min_value;
  }
  return std::clamp(value, min_value, max_value);
}

auto IsFiniteMatrix(const cv::Matx33d& m) -> bool {
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      if (!std::isfinite(m(r, c))) {
        return false;
      }
    }
  }
  return true;
}

void HashCombine(std::uint64_t& seed, std::uint64_t value) {
  seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
}

auto FloatHashBits(float value) -> std::uint64_t {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

void HashFloatArray(std::uint64_t& seed, const float* values, int count) {
  for (int i = 0; i < count; ++i) {
    HashCombine(seed, FloatHashBits(values[i]));
  }
}

auto BuildRuntimeCacheKey(const OperatorParams& params, ColorTempMode mode, float custom_cct,
                          float custom_tint) -> std::uint64_t {
  std::uint64_t key = 0xcbf29ce484222325ULL;
  HashCombine(key, static_cast<std::uint64_t>(params.color_temp_enabled_));
  HashCombine(key, static_cast<std::uint64_t>(params.raw_runtime_valid_));
  HashCombine(key, static_cast<std::uint64_t>(mode));
  HashCombine(key, FloatHashBits(custom_cct));
  HashCombine(key, FloatHashBits(custom_tint));
  HashCombine(key, static_cast<std::uint64_t>(params.raw_decode_input_space_));
  HashCombine(key, static_cast<std::uint64_t>(std::hash<std::string>{}(params.raw_camera_make_)));
  HashCombine(key, static_cast<std::uint64_t>(std::hash<std::string>{}(params.raw_camera_model_)));
  HashFloatArray(key, params.raw_pre_mul_, 3);
  HashFloatArray(key, params.raw_cam_xyz_, 9);
  HashCombine(key, static_cast<std::uint64_t>(params.raw_color_matrices_valid_));
  if (params.raw_color_matrices_valid_) {
    for (double value : params.raw_color_matrix_1_) {
      HashCombine(key, static_cast<std::uint64_t>(std::hash<double>{}(value)));
    }
    for (double value : params.raw_color_matrix_2_) {
      HashCombine(key, static_cast<std::uint64_t>(std::hash<double>{}(value)));
    }
    HashCombine(key, static_cast<std::uint64_t>(params.raw_calibration_illuminants_valid_));
    HashCombine(key, static_cast<std::uint64_t>(std::hash<double>{}(params.raw_color_matrix_1_cct_)));
    HashCombine(key, static_cast<std::uint64_t>(std::hash<double>{}(params.raw_color_matrix_2_cct_)));
  }
  HashCombine(key, static_cast<std::uint64_t>(params.raw_forward_matrices_valid_));
  if (params.raw_forward_matrices_valid_) {
    for (double value : params.raw_forward_matrix_1_) {
      HashCombine(key, static_cast<std::uint64_t>(std::hash<double>{}(value)));
    }
    for (double value : params.raw_forward_matrix_2_) {
      HashCombine(key, static_cast<std::uint64_t>(std::hash<double>{}(value)));
    }
  }
  HashCombine(key, static_cast<std::uint64_t>(params.raw_as_shot_neutral_valid_));
  if (params.raw_as_shot_neutral_valid_) {
    for (double value : params.raw_as_shot_neutral_) {
      HashCombine(key, static_cast<std::uint64_t>(std::hash<double>{}(value)));
    }
  }
  if (mode == ColorTempMode::AS_SHOT) {
    HashFloatArray(key, params.raw_cam_mul_, 3);
  }
  return key;
}

auto HasValidCamXyz(const float m[9]) -> bool {
  double sum = 0.0;
  for (int i = 0; i < 9; ++i) {
    if (!std::isfinite(m[i])) {
      return false;
    }
    sum += std::abs(static_cast<double>(m[i]));
  }
  return sum > kValueEpsilon;
}

auto BuildFallbackXyzToCamera(const OperatorParams& params, cv::Matx33d& out) -> bool {
  if (!HasValidCamXyz(params.raw_cam_xyz_)) {
    return false;
  }
  const double g = std::max(static_cast<double>(params.raw_pre_mul_[1]), kValueEpsilon);
  const cv::Matx33d pre =
      cv::Matx33d::diag(cv::Vec3d(params.raw_pre_mul_[0] / g, 1.0, params.raw_pre_mul_[2] / g));
  const cv::Matx33d cam_xyz(params.raw_cam_xyz_[0], params.raw_cam_xyz_[1], params.raw_cam_xyz_[2],
                            params.raw_cam_xyz_[3], params.raw_cam_xyz_[4], params.raw_cam_xyz_[5],
                            params.raw_cam_xyz_[6], params.raw_cam_xyz_[7], params.raw_cam_xyz_[8]);
  out = pre * cam_xyz;
  return IsFiniteMatrix(out);
}

auto DevelopPayloadFromColorTemp(const OperatorParams& params, ColorTempMode mode, float custom_cct,
                                 float custom_tint) -> DevelopPayload {
  DevelopPayload develop;
  develop.wb_mode     = (mode == ColorTempMode::CUSTOM) ? "custom" : "as_shot";
  develop.custom_cct  = custom_cct;
  develop.custom_tint = custom_tint;
  auto& profile       = develop.camera_profile;
  profile.color_matrices_valid          = params.raw_color_matrices_valid_;
  profile.forward_matrices_valid        = params.raw_forward_matrices_valid_;
  profile.as_shot_neutral_valid         = params.raw_as_shot_neutral_valid_;
  profile.calibration_illuminants_valid = params.raw_calibration_illuminants_valid_;
  profile.color_matrix_1_cct            = params.raw_color_matrix_1_cct_;
  profile.color_matrix_2_cct            = params.raw_color_matrix_2_cct_;
  for (int i = 0; i < 9; ++i) {
    profile.color_matrix_1[static_cast<std::size_t>(i)]   = params.raw_color_matrix_1_[i];
    profile.color_matrix_2[static_cast<std::size_t>(i)]   = params.raw_color_matrix_2_[i];
    profile.forward_matrix_1[static_cast<std::size_t>(i)] = params.raw_forward_matrix_1_[i];
    profile.forward_matrix_2[static_cast<std::size_t>(i)] = params.raw_forward_matrix_2_[i];
  }
  for (int i = 0; i < 3; ++i) {
    profile.as_shot_neutral[static_cast<std::size_t>(i)] = params.raw_as_shot_neutral_[i];
    profile.cam_mul[static_cast<std::size_t>(i)]        = params.raw_cam_mul_[i];
  }
  return develop;
}

auto ApplyCamXyzFallback(const OperatorParams& params, DevelopPayload& develop) -> bool {
  cv::Matx33d fallback;
  if (!BuildFallbackXyzToCamera(params, fallback)) {
    return false;
  }
  auto& profile                         = develop.camera_profile;
  profile.color_matrices_valid          = true;
  profile.forward_matrices_valid        = false;
  profile.calibration_illuminants_valid = false;
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      const double value = fallback(r, c);
      profile.color_matrix_1[static_cast<std::size_t>(r * 3 + c)] = value;
      profile.color_matrix_2[static_cast<std::size_t>(r * 3 + c)] = value;
    }
  }
  return true;
}

void CopyMatrix(const std::array<float, 9>& src, float dst[9]) {
  std::memcpy(dst, src.data(), 9 * sizeof(float));
}

}  // namespace

ColorTempOp::ColorTempOp(const nlohmann::json& params) { SetParams(params); }

auto ColorTempOp::ParseMode(const std::string& mode) -> ColorTempMode {
  if (mode == "custom") {
    return ColorTempMode::CUSTOM;
  }
  if (mode == "as-shot" || mode == "as_shot") {
    return ColorTempMode::AS_SHOT;
  }
  return ColorTempMode::AS_SHOT;
}

auto ColorTempOp::ModeToString(ColorTempMode mode) -> std::string {
  switch (mode) {
    case ColorTempMode::CUSTOM:
      return "custom";
    case ColorTempMode::AS_SHOT:
    default:
      return "as_shot";
  }
}

void ColorTempOp::Apply(std::shared_ptr<ImageBuffer>) {
  throw std::runtime_error(
      "ColorTempOp: descriptor-only operator. Runtime matrices are resolved into global params.");
}

void ColorTempOp::ApplyGPU(std::shared_ptr<ImageBuffer>) {
  throw std::runtime_error(
      "ColorTempOp: descriptor-only operator. Runtime matrices are resolved into global params.");
}

auto ColorTempOp::GetParams() const -> nlohmann::json {
  nlohmann::json out;
  out[std::string(script_name_)] = {{"mode", ModeToString(mode_)},
                                    {"custom_cct", custom_cct_},
                                    {"custom_tint", custom_tint_},
                                    {"as_shot_cct", resolved_cct_},
                                    {"as_shot_tint", resolved_tint_}};
  return out;
}

void ColorTempOp::SetParams(const nlohmann::json& params) {
  if (!params.contains(script_name_)) {
    return;
  }

  const auto& j = params[script_name_];
  if (j.contains("mode") && j["mode"].is_string()) {
    mode_ = ParseMode(j["mode"].get<std::string>());
  }

  if (j.contains("custom_cct")) {
    custom_cct_ = static_cast<float>(
        ClampFinite(j["custom_cct"].get<double>(), kCustomCCTMin, kCustomCCTMax));
  } else if (j.contains("cct") && mode_ == ColorTempMode::CUSTOM) {
    custom_cct_ =
        static_cast<float>(ClampFinite(j["cct"].get<double>(), kCustomCCTMin, kCustomCCTMax));
  }

  if (j.contains("custom_tint")) {
    custom_tint_ = static_cast<float>(
        ClampFinite(j["custom_tint"].get<double>(), kCustomTintMin, kCustomTintMax));
  } else if (j.contains("tint") && mode_ == ColorTempMode::CUSTOM) {
    custom_tint_ =
        static_cast<float>(ClampFinite(j["tint"].get<double>(), kCustomTintMin, kCustomTintMax));
  }

  if (j.contains("as_shot_cct")) {
    resolved_cct_ = static_cast<float>(
        ClampFinite(j["as_shot_cct"].get<double>(), kCustomCCTMin, kCustomCCTMax));
  } else if (j.contains("resolved_cct")) {
    resolved_cct_ = static_cast<float>(
        ClampFinite(j["resolved_cct"].get<double>(), kCustomCCTMin, kCustomCCTMax));
  } else if (j.contains("cct") && mode_ == ColorTempMode::AS_SHOT) {
    resolved_cct_ =
        static_cast<float>(ClampFinite(j["cct"].get<double>(), kCustomCCTMin, kCustomCCTMax));
  }

  if (j.contains("as_shot_tint")) {
    resolved_tint_ = static_cast<float>(
        ClampFinite(j["as_shot_tint"].get<double>(), kCustomTintMin, kCustomTintMax));
  } else if (j.contains("resolved_tint")) {
    resolved_tint_ = static_cast<float>(
        ClampFinite(j["resolved_tint"].get<double>(), kCustomTintMin, kCustomTintMax));
  } else if (j.contains("tint") && mode_ == ColorTempMode::AS_SHOT) {
    resolved_tint_ =
        static_cast<float>(ClampFinite(j["tint"].get<double>(), kCustomTintMin, kCustomTintMax));
  }
}

namespace {

auto ColorTempInner(const nlohmann::json& params) -> nlohmann::json {
  if (params.contains("color_temp") && params["color_temp"].is_object()) {
    return params["color_temp"];
  }
  return nlohmann::json::object();
}

auto ColorTempModeFromParams(const nlohmann::json& params) -> ColorTempMode {
  const auto inner = ColorTempInner(params);
  if (!inner.contains("mode") || !inner["mode"].is_string()) {
    return ColorTempMode::AS_SHOT;
  }
  const auto mode = inner["mode"].get<std::string>();
  if (mode == "custom") {
    return ColorTempMode::CUSTOM;
  }
  return ColorTempMode::AS_SHOT;
}

auto ColorTempAsShotBaseline(const nlohmann::json& params, double& out_cct, double& out_tint)
    -> void {
  const auto inner = ColorTempInner(params);
  if (inner.contains("as_shot_cct") && inner["as_shot_cct"].is_number()) {
    out_cct = inner["as_shot_cct"].get<double>();
  } else if (inner.contains("resolved_cct") && inner["resolved_cct"].is_number()) {
    out_cct = inner["resolved_cct"].get<double>();
  } else if (inner.contains("cct") && inner["cct"].is_number() &&
             ColorTempModeFromParams(params) == ColorTempMode::AS_SHOT) {
    out_cct = inner["cct"].get<double>();
  } else {
    out_cct = 6500.0;
  }
  if (inner.contains("as_shot_tint") && inner["as_shot_tint"].is_number()) {
    out_tint = inner["as_shot_tint"].get<double>();
  } else if (inner.contains("resolved_tint") && inner["resolved_tint"].is_number()) {
    out_tint = inner["resolved_tint"].get<double>();
  } else if (inner.contains("tint") && inner["tint"].is_number() &&
             ColorTempModeFromParams(params) == ColorTempMode::AS_SHOT) {
    out_tint = inner["tint"].get<double>();
  } else {
    out_tint = 0.0;
  }
}

auto ColorTempCustomCct(const nlohmann::json& inner) -> double {
  if (inner.contains("custom_cct") && inner["custom_cct"].is_number()) {
    return inner["custom_cct"].get<double>();
  }
  if (inner.contains("cct") && inner["cct"].is_number()) {
    return inner["cct"].get<double>();
  }
  return 6500.0;
}

auto ColorTempCustomTint(const nlohmann::json& inner) -> double {
  if (inner.contains("custom_tint") && inner["custom_tint"].is_number()) {
    return inner["custom_tint"].get<double>();
  }
  if (inner.contains("tint") && inner["tint"].is_number()) {
    return inner["tint"].get<double>();
  }
  return 0.0;
}

}  // namespace

auto ColorTempOp::DetectMergeConflict(const nlohmann::json& current,
                                      const nlohmann::json& incoming) const -> bool {
  const auto current_mode  = ColorTempModeFromParams(current);
  const auto incoming_mode = ColorTempModeFromParams(incoming);
  if (current_mode == ColorTempMode::AS_SHOT && incoming_mode == ColorTempMode::AS_SHOT) {
    return false;
  }
  if (current_mode != incoming_mode) {
    return true;
  }
  const auto cur = ColorTempInner(current);
  const auto inc = ColorTempInner(incoming);
  const double cur_cct  = ColorTempCustomCct(cur);
  const double cur_tint = ColorTempCustomTint(cur);
  const double inc_cct  = ColorTempCustomCct(inc);
  const double inc_tint = ColorTempCustomTint(inc);
  constexpr double kCctEps  = 0.5;
  constexpr double kTintEps = 0.05;
  return std::abs(cur_cct - inc_cct) > kCctEps || std::abs(cur_tint - inc_tint) > kTintEps;
}

auto ColorTempOp::MergeParams(const nlohmann::json& current, const nlohmann::json& incoming,
                              OperatorMergeChoice choice) const -> nlohmann::json {
  if (choice == OperatorMergeChoice::kKeepCurrent) {
    return current;
  }

  const auto incoming_mode = ColorTempModeFromParams(incoming);
  nlohmann::json result =
      current.is_object() ? current : nlohmann::json{{std::string(script_name_), nlohmann::json::object()}};
  if (!result.contains(std::string(script_name_)) || !result[std::string(script_name_)].is_object()) {
    result[std::string(script_name_)] = nlohmann::json::object();
  }
  auto& out = result[std::string(script_name_)];
  const auto cur = ColorTempInner(current);

  if (incoming_mode == ColorTempMode::AS_SHOT) {
    double baseline_cct  = 6500.0;
    double baseline_tint = 0.0;
    ColorTempAsShotBaseline(current, baseline_cct, baseline_tint);
    out["mode"]         = "as_shot";
    out["as_shot_cct"]  = baseline_cct;
    out["as_shot_tint"] = baseline_tint;
    out["custom_cct"]   = ColorTempCustomCct(cur);
    out["custom_tint"]  = ColorTempCustomTint(cur);
    return result;
  }

  const auto inc = ColorTempInner(incoming);
  double baseline_cct  = 6500.0;
  double baseline_tint = 0.0;
  ColorTempAsShotBaseline(current, baseline_cct, baseline_tint);
  out["mode"]         = "custom";
  out["custom_cct"]   = ColorTempCustomCct(inc);
  out["custom_tint"]  = ColorTempCustomTint(inc);
  out["as_shot_cct"]  = baseline_cct;
  out["as_shot_tint"] = baseline_tint;
  return result;
}

void ColorTempOp::SetGlobalParams(OperatorParams& params) const {
  params.color_temp_mode_          = mode_;
  params.color_temp_custom_cct_    = custom_cct_;
  params.color_temp_custom_tint_   = custom_tint_;
  params.color_temp_resolved_cct_  = resolved_cct_;
  params.color_temp_resolved_tint_ = resolved_tint_;
  params.color_temp_runtime_dirty_ = true;
  ResolveRuntime(params);
}

void ColorTempOp::EnableGlobalParams(OperatorParams& params, bool enable) {
  params.color_temp_enabled_       = enable;
  params.color_temp_runtime_dirty_ = true;
}

void ColorTempOp::ResolveRuntime(OperatorParams& params) const {
  params.color_temp_mode_        = mode_;
  params.color_temp_custom_cct_  = custom_cct_;
  params.color_temp_custom_tint_ = custom_tint_;

  const std::uint64_t runtime_cache_key =
      BuildRuntimeCacheKey(params, mode_, custom_cct_, custom_tint_);
  if (params.color_temp_cache_key_valid_ && params.color_temp_cache_key_ == runtime_cache_key) {
    resolved_cct_                    = params.color_temp_resolved_cct_;
    resolved_tint_                   = params.color_temp_resolved_tint_;
    params.color_temp_runtime_dirty_ = false;
    return;
  }
  params.color_temp_cache_key_       = runtime_cache_key;
  params.color_temp_cache_key_valid_ = true;

  if (!params.color_temp_enabled_ || !params.raw_runtime_valid_) {
    params.color_temp_matrices_valid_ = false;
    params.color_temp_runtime_dirty_  = false;
    return;
  }

  auto payload = DevelopPayloadFromColorTemp(params, mode_, custom_cct_, custom_tint_);
  auto result  = ResolveDevelopColorTransform(payload);
  if (!result.ok && result.error == ColorTransformError::MissingCameraMatrices &&
      ApplyCamXyzFallback(params, payload)) {
    result = ResolveDevelopColorTransform(payload);
  }
  if (!result.ok) {
    params.color_temp_matrices_valid_ = false;
    params.color_temp_runtime_dirty_  = false;
    return;
  }

  CopyMatrix(result.transform.camera_to_xyz, params.color_temp_cam_to_xyz_);
  CopyMatrix(result.transform.camera_to_xyz_d50, params.color_temp_cam_to_xyz_d50_);
  CopyMatrix(result.transform.xyz_d50_to_ap1, params.color_temp_xyz_d50_to_ap1_);
  CopyMatrix(result.transform.camera_to_ap1, params.color_temp_cam_to_ap1_);

  resolved_cct_  = result.transform.resolved_cct;
  resolved_tint_ = result.transform.resolved_tint;
  params.color_temp_custom_cct_      = resolved_cct_;
  params.color_temp_custom_tint_     = resolved_tint_;
  params.color_temp_resolved_cct_    = resolved_cct_;
  params.color_temp_resolved_tint_   = resolved_tint_;
  params.color_temp_runtime_dirty_   = false;
  params.color_temp_matrices_valid_  = true;
}
}  // namespace alcedo
