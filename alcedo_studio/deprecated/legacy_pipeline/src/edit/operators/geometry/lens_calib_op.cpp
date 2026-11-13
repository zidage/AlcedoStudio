//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/geometry/lens_calib_op.hpp"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#ifdef HAVE_CUDA
#include "edit/runtime/lens/cuda/cuda_lens_calib_ops.hpp"
#endif
#ifdef HAVE_METAL
#include "edit/runtime/lens/metal/metal_lens_calib.hpp"
#endif
#ifdef HAVE_OPENCL
#include "edit/runtime/lens/opencl/opencl_lens_calib_ops.hpp"
#endif
#include "utils/string/convert.hpp"

namespace alcedo {
namespace {

auto IsFinitePositive(float value) -> bool { return std::isfinite(value) && value > 0.0f; }

auto TrimWhitespace(std::string text) -> std::string {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
    text.erase(text.begin());
  }
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
    text.pop_back();
  }
  return text;
}

auto StripWrappedQuotes(std::string text) -> std::string {
  text = TrimWhitespace(std::move(text));
  while (text.size() >= 2) {
    const char first = text.front();
    const char last  = text.back();
    if (!((first == '"' && last == '"') || (first == '\'' && last == '\''))) {
      break;
    }
    text = TrimWhitespace(text.substr(1, text.size() - 2));
  }
  return text;
}

void HashCombine(std::uint64_t& seed, std::uint64_t value) {
  seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U);
}

auto HashFloatBits(float value) -> std::uint64_t {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

}  // namespace

LensCalibOp::LensCalibOp(const nlohmann::json& params) { SetParams(params); }

auto LensCalibOp::CorrectionSettings() const -> LensCorrectionSettings {
  LensCorrectionSettings settings;
  settings.database_path_         = lens_profile_db_path_;
  settings.apply_vignetting_      = apply_vignetting_;
  settings.apply_distortion_      = apply_distortion_;
  settings.apply_tca_             = apply_tca_;
  settings.apply_crop_            = apply_crop_;
  settings.auto_scale_            = auto_scale_;
  settings.use_user_scale_        = use_user_scale_;
  settings.user_scale_            = user_scale_;
  settings.projection_enabled_    = projection_enabled_;
  settings.target_projection_     = target_projection_;
  settings.low_precision_preview_ = low_precision_preview_;
  return settings;
}

void LensCalibOp::BindImageExtent(int width, int height) const {
  LensCalibrationResolver::BindImageExtent(resolved_input_meta_, lens_profile_db_path_,
                                           resolved_params_, width, height);
}

void LensCalibOp::Apply(std::shared_ptr<ImageBuffer>) {
  // CPU builds keep lens calibration as a no-op. GPU backends are implemented
  // in ApplyGPU().
}

void LensCalibOp::ApplyGPU(std::shared_ptr<ImageBuffer> input) {
  if (!enabled_ || !has_resolved_params_ || !input) {
    return;
  }
  if (!input->gpu_data_valid_) {
    input->SyncToGPU();
  }

#if !defined(HAVE_CUDA) && !defined(HAVE_METAL) && !defined(HAVE_OPENCL)
  throw std::runtime_error("LensCalibOp::ApplyGPU requires HAVE_CUDA, HAVE_METAL, or HAVE_OPENCL");
#else

#ifdef HAVE_OPENCL
  if (input->GetGPUBackend() == GpuBackendKind::OpenCL) {
    auto& gpu = input->GetOpenClImage();
    if (gpu.Empty()) {
      std::cout << "LensCalibOp: Input GPU data is empty, skipping lens calibration." << std::endl;
      return;
    }
    BindImageExtent(gpu.Width(), gpu.Height());
    OpenCL::Geometry::ApplyLensCalibration(gpu, resolved_params_);
    input->gpu_data_valid_ = true;
    return;
  }
#endif

#ifdef HAVE_CUDA
  if (input->GetGPUBackend() == GpuBackendKind::CUDA) {
    auto& gpu = input->GetCUDAImage();
    if (gpu.empty()) {
      std::cout << "LensCalibOp: Input GPU data is empty, skipping lens calibration." << std::endl;
      return;
    }
    BindImageExtent(gpu.cols, gpu.rows);
    CUDA::ApplyLensCalibration(gpu, resolved_params_);
    input->gpu_data_valid_ = true;
    return;
  }
#endif

#ifdef HAVE_METAL
  if (input->GetGPUBackend() == GpuBackendKind::Metal) {
    auto& gpu = input->GetMetalImage();
    if (gpu.Empty()) {
      std::cout << "LensCalibOp: Input GPU data is empty, skipping lens calibration." << std::endl;
      return;
    }
    BindImageExtent(static_cast<int>(gpu.Width()), static_cast<int>(gpu.Height()));
    metal::ApplyLensCalibration(gpu, resolved_params_);
    input->gpu_data_valid_ = true;
    return;
  }
#endif

  throw std::runtime_error("LensCalibOp::ApplyGPU: active GPU backend is not supported");
#endif
}

auto LensCalibOp::GetParams() const -> nlohmann::json {
  nlohmann::json inner;
  inner["enabled"]               = enabled_;
  inner["apply_vignetting"]      = apply_vignetting_;
  inner["apply_distortion"]      = apply_distortion_;
  inner["apply_tca"]             = apply_tca_;
  inner["apply_crop"]            = apply_crop_;
  inner["auto_scale"]            = auto_scale_;
  inner["use_user_scale"]        = use_user_scale_;
  inner["user_scale"]            = user_scale_;
  inner["projection_enabled"]    = projection_enabled_;
  inner["target_projection"]     = target_projection_;
  inner["low_precision_preview"] = low_precision_preview_;

  inner["cam_maker"]             = input_meta_.cam_maker_;
  inner["cam_model"]             = input_meta_.cam_model_;
  inner["lens_maker"]            = input_meta_.lens_maker_;
  inner["lens_model"]            = input_meta_.lens_model_;
  inner["focal_length_mm"]       = input_meta_.focal_length_mm_;
  inner["aperture_f_number"]     = input_meta_.aperture_f_number_;
  inner["distance_m"]            = input_meta_.distance_m_;
  inner["focal_35mm_mm"]         = input_meta_.focal_35mm_mm_;
  inner["crop_factor_hint"]      = input_meta_.crop_factor_hint_;
  inner["lens_profile_db_path"]  = conv::ToBytes(lens_profile_db_path_.wstring());
  return {{std::string(script_name_), std::move(inner)}};
}

void LensCalibOp::SetParams(const nlohmann::json& params) {
  nlohmann::json inner =
      params.contains(script_name_) ? params[script_name_] : nlohmann::json::object();
  enabled_                       = inner.value("enabled", enabled_);
  apply_vignetting_              = inner.value("apply_vignetting", apply_vignetting_);
  apply_distortion_              = inner.value("apply_distortion", apply_distortion_);
  apply_tca_                     = inner.value("apply_tca", apply_tca_);
  apply_crop_                    = inner.value("apply_crop", apply_crop_);
  auto_scale_                    = inner.value("auto_scale", auto_scale_);
  use_user_scale_                = inner.value("use_user_scale", use_user_scale_);
  user_scale_                    = inner.value("user_scale", user_scale_);
  projection_enabled_            = inner.value("projection_enabled", projection_enabled_);
  target_projection_             = inner.value("target_projection", target_projection_);
  low_precision_preview_         = inner.value("low_precision_preview", low_precision_preview_);

  // Image-local EXIF/runtime metadata: keep existing values when the transfer
  // package omits them (SanitizeLens strips these keys on capture).
  input_meta_.cam_maker_         = inner.value("cam_maker", input_meta_.cam_maker_);
  input_meta_.cam_model_         = inner.value("cam_model", input_meta_.cam_model_);
  input_meta_.lens_maker_        = inner.value("lens_maker", input_meta_.lens_maker_);
  input_meta_.lens_model_        = inner.value("lens_model", input_meta_.lens_model_);
  input_meta_.focal_length_mm_   = inner.value("focal_length_mm", input_meta_.focal_length_mm_);
  input_meta_.aperture_f_number_ = inner.value("aperture_f_number", input_meta_.aperture_f_number_);
  input_meta_.distance_m_        = inner.value("distance_m", input_meta_.distance_m_);
  input_meta_.focal_35mm_mm_     = inner.value("focal_35mm_mm", input_meta_.focal_35mm_mm_);
  input_meta_.crop_factor_hint_  = inner.value("crop_factor_hint", input_meta_.crop_factor_hint_);

  if (inner.contains("lens_profile_db_path")) {
    std::string configured = inner.value("lens_profile_db_path", std::string{});
    configured             = StripWrappedQuotes(std::move(configured));
    lens_profile_db_path_  = configured.empty() ? std::filesystem::path{}
                                                : std::filesystem::path(conv::FromBytes(configured));
  }
  if (lens_profile_db_path_.empty()) {
    lens_profile_db_path_ = LensCalibrationResolver::DefaultDatabasePath();
  }
  has_resolved_params_ = false;
  resolved_input_meta_ = {};
}

namespace {

auto LensCalibInner(const nlohmann::json& params) -> nlohmann::json {
  if (params.contains("lens_calib") && params["lens_calib"].is_object()) {
    return params["lens_calib"];
  }
  return nlohmann::json::object();
}

auto IsLensImageLocalKey(std::string_view key) -> bool {
  return key == "cam_maker" || key == "cam_model" || key == "lens_maker" || key == "lens_model" ||
         key == "focal_length_mm" || key == "aperture_f_number" || key == "distance_m" ||
         key == "focal_35mm_mm" || key == "crop_factor_hint" || key == "lens_profile_db_path";
}

auto LensPortableParams(const nlohmann::json& params) -> nlohmann::json {
  const auto inner = LensCalibInner(params);
  nlohmann::json portable = nlohmann::json::object();
  for (const auto& [key, value] : inner.items()) {
    if (!IsLensImageLocalKey(key)) {
      portable[key] = value;
    }
  }
  return portable;
}

}  // namespace

auto LensCalibOp::DetectMergeConflict(const nlohmann::json& current,
                                      const nlohmann::json& incoming) const -> bool {
  // Only portable correction intent participates in merge conflicts. EXIF /
  // profile paths stay with the target image and must not force a conflict.
  return LensPortableParams(current) != LensPortableParams(incoming);
}

auto LensCalibOp::MergeParams(const nlohmann::json& current, const nlohmann::json& incoming,
                              OperatorMergeChoice choice) const -> nlohmann::json {
  if (choice == OperatorMergeChoice::kKeepCurrent) {
    return current;
  }

  nlohmann::json result =
      current.is_object() ? current : nlohmann::json{{std::string(script_name_), nlohmann::json::object()}};
  if (!result.contains(std::string(script_name_)) || !result[std::string(script_name_)].is_object()) {
    result[std::string(script_name_)] = nlohmann::json::object();
  }
  auto& out = result[std::string(script_name_)];
  const auto inc = LensCalibInner(incoming);
  for (const auto& [key, value] : inc.items()) {
    if (!IsLensImageLocalKey(key)) {
      out[key] = value;
    }
  }
  return result;
}

auto LensCalibOp::BuildRuntimeCacheKey(const OperatorParams& params) const -> uint64_t {
  std::uint64_t key = 0xcbf29ce484222325ULL;
  HashCombine(key, static_cast<std::uint64_t>(enabled_));
  HashCombine(key, static_cast<std::uint64_t>(apply_vignetting_));
  HashCombine(key, static_cast<std::uint64_t>(apply_distortion_));
  HashCombine(key, static_cast<std::uint64_t>(apply_tca_));
  HashCombine(key, static_cast<std::uint64_t>(apply_crop_));
  HashCombine(key, static_cast<std::uint64_t>(auto_scale_));
  HashCombine(key, static_cast<std::uint64_t>(use_user_scale_));
  HashCombine(key, HashFloatBits(user_scale_));
  HashCombine(key, static_cast<std::uint64_t>(projection_enabled_));
  HashCombine(key, static_cast<std::uint64_t>(std::hash<std::string>{}(target_projection_)));
  HashCombine(key, static_cast<std::uint64_t>(low_precision_preview_));
  HashCombine(key, static_cast<std::uint64_t>(std::hash<std::string>{}(params.raw_camera_make_)));
  HashCombine(key, static_cast<std::uint64_t>(std::hash<std::string>{}(params.raw_camera_model_)));
  HashCombine(key, static_cast<std::uint64_t>(std::hash<std::string>{}(params.raw_lens_make_)));
  HashCombine(key, static_cast<std::uint64_t>(std::hash<std::string>{}(params.raw_lens_model_)));
  HashCombine(key, HashFloatBits(params.raw_lens_focal_mm_));
  HashCombine(key, HashFloatBits(params.raw_lens_aperture_f_));
  HashCombine(key, HashFloatBits(params.raw_lens_focus_distance_m_));
  HashCombine(key, HashFloatBits(params.raw_lens_focal_35mm_));
  HashCombine(key, HashFloatBits(params.raw_lens_crop_factor_hint_));
  HashCombine(key, static_cast<std::uint64_t>(params.raw_dng_warp_rectilinear_present_));
  HashCombine(key, static_cast<std::uint64_t>(std::hash<std::string>{}(input_meta_.cam_maker_)));
  HashCombine(key, static_cast<std::uint64_t>(std::hash<std::string>{}(input_meta_.cam_model_)));
  HashCombine(key, static_cast<std::uint64_t>(std::hash<std::string>{}(input_meta_.lens_maker_)));
  HashCombine(key, static_cast<std::uint64_t>(std::hash<std::string>{}(input_meta_.lens_model_)));
  HashCombine(key, HashFloatBits(input_meta_.focal_length_mm_));
  HashCombine(key, HashFloatBits(input_meta_.aperture_f_number_));
  HashCombine(key, HashFloatBits(input_meta_.distance_m_));
  HashCombine(key, HashFloatBits(input_meta_.focal_35mm_mm_));
  HashCombine(key, HashFloatBits(input_meta_.crop_factor_hint_));
  HashCombine(key,
              static_cast<std::uint64_t>(std::hash<std::string>{}(lens_profile_db_path_.string())));
  return key;
}

static void PrintResolvedParams(const OperatorParams& params) {
  std::cout << "Current raw parameters: " << std::endl;
  std::cout << "  Camera Make: " << params.raw_camera_make_ << std::endl;
  std::cout << "  Camera Model: " << params.raw_camera_model_ << std::endl;
  std::cout << "  Lens Make: " << params.raw_lens_make_ << std::endl;
  std::cout << "  Lens Model: " << params.raw_lens_model_ << std::endl;
  std::cout << "  Focal Length (mm): " << params.raw_lens_focal_mm_ << std::endl;
  std::cout << "  Aperture (f-number): " << params.raw_lens_aperture_f_ << std::endl;
  std::cout << "  Focus Distance (m): " << params.raw_lens_focus_distance_m_ << std::endl;
  std::cout << "  Crop Factor Hint: " << params.raw_lens_crop_factor_hint_ << std::endl;
}

void LensCalibOp::ResolveRuntime(OperatorParams& params) const {
  if (!params.raw_runtime_valid_) {
    params.lens_calib_runtime_valid_  = false;
    params.lens_calib_runtime_failed_ = true;
    params.lens_calib_runtime_dirty_  = false;
    has_resolved_params_              = false;
    std::cout << "Warning: Raw decoding parameters are not valid. Lens calibration will be skipped."
              << std::endl;
    PrintResolvedParams(params);
    return;
  }

  const auto cache_key = BuildRuntimeCacheKey(params);
  if (!params.lens_calib_runtime_dirty_ && params.lens_calib_cache_key_valid_ &&
      params.lens_calib_cache_key_ == cache_key) {
    resolved_params_     = params.lens_calib_runtime_params_;
    has_resolved_params_ = params.lens_calib_runtime_valid_;
    return;
  }

  params.lens_calib_cache_key_valid_ = true;
  params.lens_calib_cache_key_       = cache_key;

  LensInputMeta meta;
  meta.cam_maker_         = params.raw_camera_make_;
  meta.cam_model_         = params.raw_camera_model_;
  meta.lens_maker_        = params.raw_lens_make_;
  meta.lens_model_        = params.raw_lens_model_;
  meta.focal_length_mm_   = params.raw_lens_focal_mm_;
  meta.aperture_f_number_ = params.raw_lens_aperture_f_;
  meta.distance_m_        = params.raw_lens_focus_distance_m_;
  meta.focal_35mm_mm_     = params.raw_lens_focal_35mm_;
  meta.crop_factor_hint_  = params.raw_lens_crop_factor_hint_;

  if (!input_meta_.cam_maker_.empty()) {
    meta.cam_maker_ = input_meta_.cam_maker_;
  }
  if (!input_meta_.cam_model_.empty()) {
    meta.cam_model_ = input_meta_.cam_model_;
  }
  if (!input_meta_.lens_maker_.empty()) {
    meta.lens_maker_ = input_meta_.lens_maker_;
  }
  if (!input_meta_.lens_model_.empty()) {
    meta.lens_model_ = input_meta_.lens_model_;
  }
  if (IsFinitePositive(input_meta_.focal_length_mm_)) {
    meta.focal_length_mm_ = input_meta_.focal_length_mm_;
  }
  if (IsFinitePositive(input_meta_.aperture_f_number_)) {
    meta.aperture_f_number_ = input_meta_.aperture_f_number_;
  }
  if (IsFinitePositive(input_meta_.distance_m_)) {
    meta.distance_m_ = input_meta_.distance_m_;
  }
  if (IsFinitePositive(input_meta_.focal_35mm_mm_)) {
    meta.focal_35mm_mm_ = input_meta_.focal_35mm_mm_;
  }
  if (IsFinitePositive(input_meta_.crop_factor_hint_)) {
    meta.crop_factor_hint_ = input_meta_.crop_factor_hint_;
  }
  // Keep input_meta_ as user overrides only. Do not persist auto-resolved metadata back into
  // operator params; otherwise UI-side param patching can churn every frame.

  ResolveRuntimeForMeta(meta, params.raw_dng_warp_rectilinear_present_, params);
}

void LensCalibOp::ResolveRuntimeForMeta(const LensInputMeta& meta, bool dng_geometry_applied,
                                        OperatorParams& owner) const {
  const auto profile =
      LensCalibrationResolver::ResolveProfile(meta, CorrectionSettings(), dng_geometry_applied);
  switch (profile.status_) {
    case LensProfileStatus::Resolved:
    case LensProfileStatus::NoCorrectionApplies: {
      const bool resolved              = profile.status_ == LensProfileStatus::Resolved;
      resolved_input_meta_             = profile.matched_meta_;
      resolved_params_                 = profile.params_;
      has_resolved_params_             = resolved;
      owner.lens_calib_runtime_valid_  = resolved;
      owner.lens_calib_runtime_failed_ = false;
      owner.lens_calib_runtime_dirty_  = false;
      owner.lens_calib_runtime_params_ = profile.params_;
      return;
    }
    case LensProfileStatus::MissingLensIdentity:
    case LensProfileStatus::DatabaseUnavailable:
    case LensProfileStatus::LensNotInDatabase:
      owner.lens_calib_runtime_valid_  = false;
      owner.lens_calib_runtime_failed_ = true;
      owner.lens_calib_runtime_dirty_  = false;
      has_resolved_params_             = false;
      return;
  }
}

void LensCalibOp::SetGlobalParams(OperatorParams& params) const {
  params.lens_calib_enabled_ = enabled_;
  if (!enabled_) {
    params.lens_calib_runtime_valid_  = false;
    params.lens_calib_runtime_failed_ = false;
    params.lens_calib_runtime_dirty_  = false;
    has_resolved_params_              = false;
    return;
  }
  ResolveRuntime(params);
}

void LensCalibOp::EnableGlobalParams(OperatorParams& params, bool enable) {
  if (params.lens_calib_enabled_ == enable) {
    return;  // No state change; avoid unnecessary dirty marking.
  }
  params.lens_calib_enabled_       = enable;
  params.lens_calib_runtime_dirty_ = true;
  has_resolved_params_             = false;
}

}  // namespace alcedo
