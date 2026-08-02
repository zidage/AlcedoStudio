//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/geometry/crop_rotate_op.hpp"

#include <algorithm>
#include <cmath>
#include <opencv2/imgproc.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#ifdef HAVE_CUDA
#include "edit/operators/geometry/cuda_geometry_ops.hpp"
#endif
#ifdef HAVE_METAL
#include "metal/metal_utils/geometry_utils.hpp"
#endif
#ifdef HAVE_OPENCL
#include "opencl/opencl_geometry_utils.hpp"
#endif

namespace alcedo {
namespace {
constexpr float kAngleEpsilon = 1e-4f;
constexpr float kCropEpsilon  = 1e-4f;
constexpr float kPi           = 3.14159265358979323846f;

enum class CropAspectPreset : int {
  Free = 0,
  Custom,
  Ratio235_1_35mm,
  Ratio1_1,
  Ratio16_9,
  Ratio1_9_IMAX,
  Ratio1_85_DCI,
  Ratio2_2_70mm,
  Ratio1_43_70mm_IMAX,
  Ratio4_3_35mm,
  Ratio1_5_NativeOrVistaVision,
  Ratio2_76_PanavisionUltra,
};

auto ClampCropRect(NormalizedCropRect r) -> NormalizedCropRect {
  r.w_ = std::clamp(r.w_, kCropEpsilon, 1.0f);
  r.h_ = std::clamp(r.h_, kCropEpsilon, 1.0f);
  r.x_ = std::clamp(r.x_, 0.0f, 1.0f - r.w_);
  r.y_ = std::clamp(r.y_, 0.0f, 1.0f - r.h_);
  return r;
}

auto IsFullCropRect(const NormalizedCropRect& rect) -> bool {
  return std::abs(rect.x_) <= kCropEpsilon && std::abs(rect.y_) <= kCropEpsilon &&
         std::abs(rect.w_ - 1.0f) <= kCropEpsilon && std::abs(rect.h_ - 1.0f) <= kCropEpsilon;
}

auto NormalizeAngleDegrees(float angle_degrees) -> float {
  if (!std::isfinite(angle_degrees)) {
    return 0.0f;
  }
  angle_degrees = std::fmod(angle_degrees, 360.0f);
  if (angle_degrees > 180.0f) {
    angle_degrees -= 360.0f;
  } else if (angle_degrees < -180.0f) {
    angle_degrees += 360.0f;
  }
  return angle_degrees;
}

auto NormalizeAspectPair(float width, float height) -> std::optional<std::pair<float, float>> {
  if (!std::isfinite(width) || !std::isfinite(height) || width <= kCropEpsilon ||
      height <= kCropEpsilon) {
    return std::nullopt;
  }
  return std::pair<float, float>{std::max(width, kCropEpsilon), std::max(height, kCropEpsilon)};
}

auto ParseAspectPreset(std::string_view preset) -> std::optional<CropAspectPreset> {
  if (preset == "free") {
    return CropAspectPreset::Free;
  }
  if (preset == "custom") {
    return CropAspectPreset::Custom;
  }
  if (preset == "ratio_2_35_1_35mm") {
    return CropAspectPreset::Ratio235_1_35mm;
  }
  if (preset == "ratio_1_1") {
    return CropAspectPreset::Ratio1_1;
  }
  if (preset == "ratio_16_9") {
    return CropAspectPreset::Ratio16_9;
  }
  if (preset == "ratio_1_9_1_imax") {
    return CropAspectPreset::Ratio1_9_IMAX;
  }
  if (preset == "ratio_1_85_1_dci") {
    return CropAspectPreset::Ratio1_85_DCI;
  }
  if (preset == "ratio_2_2_1_70mm") {
    return CropAspectPreset::Ratio2_2_70mm;
  }
  if (preset == "ratio_1_43_1_70mm_imax") {
    return CropAspectPreset::Ratio1_43_70mm_IMAX;
  }
  if (preset == "ratio_4_3_35mm") {
    return CropAspectPreset::Ratio4_3_35mm;
  }
  if (preset == "ratio_1_5_1_native_vistavision") {
    return CropAspectPreset::Ratio1_5_NativeOrVistaVision;
  }
  if (preset == "ratio_2_76_1_panavision_ultra") {
    return CropAspectPreset::Ratio2_76_PanavisionUltra;
  }
  return std::nullopt;
}

auto AspectPresetId(CropAspectPreset preset) -> const char* {
  switch (preset) {
    case CropAspectPreset::Custom:
      return "custom";
    case CropAspectPreset::Ratio235_1_35mm:
      return "ratio_2_35_1_35mm";
    case CropAspectPreset::Ratio1_1:
      return "ratio_1_1";
    case CropAspectPreset::Ratio16_9:
      return "ratio_16_9";
    case CropAspectPreset::Ratio1_9_IMAX:
      return "ratio_1_9_1_imax";
    case CropAspectPreset::Ratio1_85_DCI:
      return "ratio_1_85_1_dci";
    case CropAspectPreset::Ratio2_2_70mm:
      return "ratio_2_2_1_70mm";
    case CropAspectPreset::Ratio1_43_70mm_IMAX:
      return "ratio_1_43_1_70mm_imax";
    case CropAspectPreset::Ratio4_3_35mm:
      return "ratio_4_3_35mm";
    case CropAspectPreset::Ratio1_5_NativeOrVistaVision:
      return "ratio_1_5_1_native_vistavision";
    case CropAspectPreset::Ratio2_76_PanavisionUltra:
      return "ratio_2_76_1_panavision_ultra";
    case CropAspectPreset::Free:
    default:
      return "free";
  }
}

auto PresetAspectPair(CropAspectPreset preset) -> std::optional<std::pair<float, float>> {
  switch (preset) {
    case CropAspectPreset::Ratio235_1_35mm:
      return std::pair<float, float>{2.35f, 1.0f};
    case CropAspectPreset::Ratio1_1:
      return std::pair<float, float>{1.0f, 1.0f};
    case CropAspectPreset::Ratio16_9:
      return std::pair<float, float>{16.0f, 9.0f};
    case CropAspectPreset::Ratio1_9_IMAX:
      return std::pair<float, float>{1.9f, 1.0f};
    case CropAspectPreset::Ratio1_85_DCI:
      return std::pair<float, float>{1.85f, 1.0f};
    case CropAspectPreset::Ratio2_2_70mm:
      return std::pair<float, float>{2.2f, 1.0f};
    case CropAspectPreset::Ratio1_43_70mm_IMAX:
      return std::pair<float, float>{1.43f, 1.0f};
    case CropAspectPreset::Ratio4_3_35mm:
      return std::pair<float, float>{4.0f, 3.0f};
    case CropAspectPreset::Ratio1_5_NativeOrVistaVision:
      return std::pair<float, float>{1.5f, 1.0f};
    case CropAspectPreset::Ratio2_76_PanavisionUltra:
      return std::pair<float, float>{2.76f, 1.0f};
    case CropAspectPreset::Free:
    case CropAspectPreset::Custom:
    default:
      return std::nullopt;
  }
}

auto ResolveAspectRatio(CropAspectPreset preset, float aspect_width, float aspect_height)
    -> std::optional<float> {
  if (preset == CropAspectPreset::Free) {
    return std::nullopt;
  }
  if (const auto preset_pair = PresetAspectPair(preset); preset_pair.has_value()) {
    return preset_pair->first / std::max(preset_pair->second, kCropEpsilon);
  }
  if (const auto normalized = NormalizeAspectPair(aspect_width, aspect_height);
      normalized.has_value()) {
    return normalized->first / std::max(normalized->second, kCropEpsilon);
  }
  return std::nullopt;
}

auto OrientPresetAspectToSource(CropAspectPreset preset, float aspect_ratio, int width, int height)
    -> float {
  if (preset == CropAspectPreset::Free || preset == CropAspectPreset::Custom || width <= 0 ||
      height <= 0 || std::abs(aspect_ratio - 1.0f) <= kCropEpsilon) {
    return aspect_ratio;
  }

  const bool source_portrait = width < height;
  const bool preset_portrait = aspect_ratio < 1.0f;
  if (source_portrait != preset_portrait) {
    return 1.0f / std::max(aspect_ratio, kCropEpsilon);
  }
  return aspect_ratio;
}

auto FitAspectRectInsideBounds(NormalizedCropRect rect, int width, int height, float aspect_ratio)
    -> NormalizedCropRect {
  rect = ClampCropRect(rect);
  if (width <= 0 || height <= 0) {
    return rect;
  }

  const float source_aspect =
      std::max(static_cast<float>(width) / static_cast<float>(height), kCropEpsilon);
  const float target_ratio          = std::max(aspect_ratio, kCropEpsilon);
  const float max_width_from_height = rect.h_ * (target_ratio / source_aspect);

  float       resolved_w            = rect.w_;
  float       resolved_h            = rect.h_;
  if (max_width_from_height <= rect.w_ + kCropEpsilon) {
    resolved_w = std::clamp(max_width_from_height, kCropEpsilon, rect.w_);
    resolved_h = std::clamp(resolved_w * (source_aspect / target_ratio), kCropEpsilon, rect.h_);
  } else {
    resolved_h = std::clamp(rect.w_ * (source_aspect / target_ratio), kCropEpsilon, rect.h_);
    resolved_w = std::clamp(resolved_h * (target_ratio / source_aspect), kCropEpsilon, rect.w_);
  }

  const float cx = rect.x_ + (rect.w_ * 0.5f);
  const float cy = rect.y_ + (rect.h_ * 0.5f);
  return ClampCropRect(
      {cx - (resolved_w * 0.5f), cy - (resolved_h * 0.5f), resolved_w, resolved_h});
}

void SanitizeAspectState(std::string& preset_id, float& aspect_width, float& aspect_height) {
  const auto preset = ParseAspectPreset(preset_id);
  if (preset.has_value() && *preset != CropAspectPreset::Custom) {
    if (*preset == CropAspectPreset::Free) {
      preset_id = AspectPresetId(CropAspectPreset::Free);
      if (!NormalizeAspectPair(aspect_width, aspect_height).has_value()) {
        aspect_width  = 1.0f;
        aspect_height = 1.0f;
      }
      return;
    }
    if (const auto ratio = PresetAspectPair(*preset); ratio.has_value()) {
      preset_id     = AspectPresetId(*preset);
      aspect_width  = ratio->first;
      aspect_height = ratio->second;
      return;
    }
  }

  if (NormalizeAspectPair(aspect_width, aspect_height).has_value()) {
    preset_id = AspectPresetId(CropAspectPreset::Custom);
    return;
  }

  preset_id     = AspectPresetId(CropAspectPreset::Free);
  aspect_width  = 1.0f;
  aspect_height = 1.0f;
}

auto ClampCropRectForRotation(NormalizedCropRect rect, float angle_degrees) -> NormalizedCropRect {
  rect                      = ClampCropRect(rect);

  const float angle_radians = NormalizeAngleDegrees(angle_degrees) * (kPi / 180.0f);
  const float c             = std::abs(std::cos(angle_radians));
  const float s             = std::abs(std::sin(angle_radians));

  float       cx            = rect.x_ + rect.w_ * 0.5f;
  float       cy            = rect.y_ + rect.h_ * 0.5f;
  float       hw            = rect.w_ * 0.5f;
  float       hh            = rect.h_ * 0.5f;

  const float extent_x      = (c * hw) + (s * hh);
  const float extent_y      = (s * hw) + (c * hh);
  const float room_x        = std::max(0.0f, std::min(cx, 1.0f - cx));
  const float room_y        = std::max(0.0f, std::min(cy, 1.0f - cy));

  float       scale         = 1.0f;
  if (extent_x > room_x + kCropEpsilon) {
    scale = std::min(scale, room_x / std::max(extent_x, kCropEpsilon));
  }
  if (extent_y > room_y + kCropEpsilon) {
    scale = std::min(scale, room_y / std::max(extent_y, kCropEpsilon));
  }
  if (scale < 1.0f) {
    hw = std::max(kCropEpsilon * 0.5f, hw * scale);
    hh = std::max(kCropEpsilon * 0.5f, hh * scale);
  }

  rect.w_ = std::clamp(hw * 2.0f, kCropEpsilon, 1.0f);
  rect.h_ = std::clamp(hh * 2.0f, kCropEpsilon, 1.0f);
  rect.x_ = cx - rect.w_ * 0.5f;
  rect.y_ = cy - rect.h_ * 0.5f;
  return ClampCropRect(rect);
}

auto MakeWarpBorderScalar(int channels) -> cv::Scalar {
  if (channels == 4) {
    return cv::Scalar(0.0, 0.0, 0.0, 1.0);
  }
  return cv::Scalar::all(0.0);
}

#ifdef HAVE_METAL
auto MakeMetalWarpBorderScalar(const metal::MetalImage& image) -> cv::Scalar {
  return MakeWarpBorderScalar(CV_MAT_CN(metal::MetalImage::CVTypeFromPixelFormat(image.Format())));
}
#endif

auto ComputeCropRoi(int width, int height, const NormalizedCropRect& rect) -> cv::Rect {
  const auto clamped = ClampCropRect(rect);
  const int  roi_w =
      std::clamp(static_cast<int>(std::lround(static_cast<float>(width) * clamped.w_)), 1, width);
  const int roi_h =
      std::clamp(static_cast<int>(std::lround(static_cast<float>(height) * clamped.h_)), 1, height);
  int roi_x = std::clamp(static_cast<int>(std::lround(static_cast<float>(width) * clamped.x_)), 0,
                         width - 1);
  int roi_y = std::clamp(static_cast<int>(std::lround(static_cast<float>(height) * clamped.y_)), 0,
                         height - 1);
  roi_x     = std::clamp(roi_x, 0, std::max(0, width - roi_w));
  roi_y     = std::clamp(roi_y, 0, std::max(0, height - roi_h));
  return cv::Rect(roi_x, roi_y, roi_w, roi_h);
}

auto BuildRotatedCropMatrix(int width, int height, const NormalizedCropRect& rect,
                            float angle_degrees, cv::Size& out_size) -> cv::Mat {
  const int out_w =
      std::clamp(static_cast<int>(std::lround(static_cast<float>(width) * rect.w_)), 1, width);
  const int out_h =
      std::clamp(static_cast<int>(std::lround(static_cast<float>(height) * rect.h_)), 1, height);

  const double angle_radians =
      static_cast<double>(angle_degrees) * (static_cast<double>(kPi) / 180.0);
  const double c      = std::cos(angle_radians);
  const double s      = std::sin(angle_radians);
  const double src_cx = static_cast<double>(rect.x_ + rect.w_ * 0.5f) * static_cast<double>(width);
  const double src_cy = static_cast<double>(rect.y_ + rect.h_ * 0.5f) * static_cast<double>(height);
  const double dst_cx = (static_cast<double>(out_w) - 1.0) * 0.5;
  const double dst_cy = (static_cast<double>(out_h) - 1.0) * 0.5;

  cv::Mat      matrix(2, 3, CV_64F);
  matrix.at<double>(0, 0) = c;
  matrix.at<double>(0, 1) = -s;
  matrix.at<double>(1, 0) = s;
  matrix.at<double>(1, 1) = c;
  matrix.at<double>(0, 2) = src_cx - (c * dst_cx) + (s * dst_cy);
  matrix.at<double>(1, 2) = src_cy - (s * dst_cx) - (c * dst_cy);

  out_size                = cv::Size(out_w, out_h);
  return matrix;
}

auto ResolveRuntimeCropRect(const NormalizedCropRect& rect, int width, int height,
                            const std::string& aspect_ratio_preset, float aspect_ratio_width,
                            float aspect_ratio_height) -> NormalizedCropRect {
  const auto preset = ParseAspectPreset(aspect_ratio_preset).value_or(CropAspectPreset::Free);
  const auto ratio  = ResolveAspectRatio(preset, aspect_ratio_width, aspect_ratio_height);
  if (!ratio.has_value()) {
    return ClampCropRect(rect);
  }
  return FitAspectRectInsideBounds(rect, width, height,
                                   OrientPresetAspectToSource(preset, *ratio, width, height));
}
}  // namespace

CropRotateOp::CropRotateOp(const nlohmann::json& params) { SetParams(params); }

void CropRotateOp::Apply(std::shared_ptr<ImageBuffer> input) {
  auto&     img    = input->GetCPUData();
  const int width  = img.cols;
  const int height = img.rows;
  if (width <= 0 || height <= 0) {
    return;
  }

  if (!enabled_ || !enable_crop_) {
    return;
  }

  const float angle_degrees = NormalizeAngleDegrees(angle_degrees_);
  const auto resolved_rect = ResolveRuntimeCropRect(crop_rect_, width, height, aspect_ratio_preset_,
                                                    aspect_ratio_width_, aspect_ratio_height_);
  const auto crop_rect     = ClampCropRectForRotation(resolved_rect, angle_degrees);
  const bool has_rotation  = std::abs(angle_degrees) > kAngleEpsilon;
  if (IsFullCropRect(crop_rect) && !has_rotation) {
    return;
  }

  if (!has_rotation) {
    const cv::Rect roi = ComputeCropRoi(width, height, crop_rect);
    img                = img(roi).clone();
    return;
  }

  // In rotated-crop-frame semantics, expand_to_fit is intentionally ignored.
  cv::Size out_size;
  cv::Mat  matrix = BuildRotatedCropMatrix(width, height, crop_rect, angle_degrees, out_size);
  cv::Mat  rotated_crop;
  cv::warpAffine(img, rotated_crop, matrix, out_size, cv::INTER_LINEAR | cv::WARP_INVERSE_MAP,
                 cv::BORDER_CONSTANT, MakeWarpBorderScalar(img.channels()));
  img = rotated_crop;
}

void CropRotateOp::ApplyGPU(std::shared_ptr<ImageBuffer> input) {
#if !defined(HAVE_CUDA) && !defined(HAVE_METAL) && !defined(HAVE_OPENCL)
  throw std::runtime_error("CropRotateOp::ApplyGPU requires HAVE_CUDA, HAVE_METAL, or HAVE_OPENCL");
#else
  if (!input->gpu_data_valid_) {
    input->SyncToGPU();
  }

#ifdef HAVE_OPENCL
  if (input->GetGPUBackend() == GpuBackendKind::OpenCL) {
    auto&     img    = input->GetOpenClImage();
    const int width  = img.Width();
    const int height = img.Height();
    if (width <= 0 || height <= 0) {
      return;
    }

    if (!enabled_ || !enable_crop_) {
      return;
    }

    const float angle_degrees = NormalizeAngleDegrees(angle_degrees_);
    const auto  resolved_rect = ResolveRuntimeCropRect(
        crop_rect_, width, height, aspect_ratio_preset_, aspect_ratio_width_, aspect_ratio_height_);
    const auto crop_rect    = ClampCropRectForRotation(resolved_rect, angle_degrees);
    const bool has_rotation = std::abs(angle_degrees) > kAngleEpsilon;
    if (IsFullCropRect(crop_rect) && !has_rotation) {
      return;
    }

    opencl::OpenClImage dst;
    if (!has_rotation) {
      const cv::Rect roi = ComputeCropRoi(width, height, crop_rect);
      OpenCL::Geometry::CropResize(img, dst, roi, roi.size());
      img = std::move(dst);
      return;
    }

    cv::Size out_size;
    cv::Mat  matrix = BuildRotatedCropMatrix(width, height, crop_rect, angle_degrees, out_size);
    OpenCL::Geometry::WarpAffineLinear(img, dst, matrix, out_size,
                                       MakeWarpBorderScalar(CV_MAT_CN(img.Type())));
    img = std::move(dst);
    return;
  }
#endif

#ifdef HAVE_CUDA
  if (input->GetGPUBackend() == GpuBackendKind::CUDA) {
    auto&     img    = input->GetCUDAImage();
    const int width  = img.cols;
    const int height = img.rows;
    if (width <= 0 || height <= 0) {
      return;
    }

    if (!enabled_ || !enable_crop_) {
      return;
    }

    const float angle_degrees = NormalizeAngleDegrees(angle_degrees_);
    const auto  resolved_rect = ResolveRuntimeCropRect(
        crop_rect_, width, height, aspect_ratio_preset_, aspect_ratio_width_, aspect_ratio_height_);
    const auto crop_rect    = ClampCropRectForRotation(resolved_rect, angle_degrees);
    const bool has_rotation = std::abs(angle_degrees) > kAngleEpsilon;
    if (IsFullCropRect(crop_rect) && !has_rotation) {
      return;
    }

    if (!has_rotation) {
      const cv::Rect roi = ComputeCropRoi(width, height, crop_rect);
      img                = img(roi).clone();
      return;
    }

    // In rotated-crop-frame semantics, expand_to_fit is intentionally ignored.
    cv::Size out_size;
    cv::Mat  matrix = BuildRotatedCropMatrix(width, height, crop_rect, angle_degrees, out_size);
    cv::cuda::GpuMat rotated_crop;
#ifdef HAVE_CUDA
    CUDA::WarpAffineLinear(img, rotated_crop, matrix, out_size,
                           MakeWarpBorderScalar(img.channels()));
#else
    throw std::runtime_error("CropRotateOp::ApplyGPU requires HAVE_CUDA");
#endif
    img = std::move(rotated_crop);
    return;
  }
#endif

#ifdef HAVE_METAL
  if (input->GetGPUBackend() == GpuBackendKind::Metal) {
    auto&     img    = input->GetMetalImage();
    const int width  = static_cast<int>(img.Width());
    const int height = static_cast<int>(img.Height());
    if (width <= 0 || height <= 0) {
      return;
    }

    if (!enabled_ || !enable_crop_) {
      return;
    }

    const float angle_degrees = NormalizeAngleDegrees(angle_degrees_);
    const auto  resolved_rect = ResolveRuntimeCropRect(
        crop_rect_, width, height, aspect_ratio_preset_, aspect_ratio_width_, aspect_ratio_height_);
    const auto crop_rect    = ClampCropRectForRotation(resolved_rect, angle_degrees);
    const bool has_rotation = std::abs(angle_degrees) > kAngleEpsilon;
    if (IsFullCropRect(crop_rect) && !has_rotation) {
      return;
    }

    metal::MetalImage dst;
    if (!has_rotation) {
      const cv::Rect roi = ComputeCropRoi(width, height, crop_rect);
      metal::utils::CropResizeTexture(img, dst, roi, roi.size());
      img = std::move(dst);
      return;
    }

    // In rotated-crop-frame semantics, expand_to_fit is intentionally ignored.
    cv::Size out_size;
    cv::Mat  matrix = BuildRotatedCropMatrix(width, height, crop_rect, angle_degrees, out_size);
    metal::utils::WarpAffineLinearTexture(img, dst, matrix, out_size,
                                          MakeMetalWarpBorderScalar(img));
    img = std::move(dst);
    return;
  }
#endif

  throw std::runtime_error("CropRotateOp::ApplyGPU: active GPU backend is not supported");
#endif
}

auto CropRotateOp::GetParams() const -> nlohmann::json {
  return {
      {std::string(script_name_),
       {{"enabled", enabled_},
        {"angle_degrees", angle_degrees_},
        {"enable_crop", enable_crop_},
        {"crop_rect",
         {{"x", crop_rect_.x_}, {"y", crop_rect_.y_}, {"w", crop_rect_.w_}, {"h", crop_rect_.h_}}},
        {"expand_to_fit", expand_to_fit_},
        {"aspect_ratio_preset", aspect_ratio_preset_},
        {"aspect_ratio", {{"width", aspect_ratio_width_}, {"height", aspect_ratio_height_}}}}}};
}

void CropRotateOp::SetParams(const nlohmann::json& params) {
  enabled_             = false;
  angle_degrees_       = 0.0f;
  enable_crop_         = false;
  crop_rect_           = {};
  expand_to_fit_       = true;
  aspect_ratio_preset_ = "free";
  aspect_ratio_width_  = 1.0f;
  aspect_ratio_height_ = 1.0f;

  if (!params.contains(script_name_) || !params[script_name_].is_object()) {
    return;
  }

  const auto& inner = params[script_name_];
  enabled_          = inner.value("enabled", enabled_);
  angle_degrees_    = inner.value("angle_degrees", angle_degrees_);
  enable_crop_      = inner.value("enable_crop", enable_crop_);
  expand_to_fit_    = inner.value("expand_to_fit", expand_to_fit_);
  if (inner.contains("crop_rect") && inner["crop_rect"].is_object()) {
    const auto& rect = inner["crop_rect"];
    crop_rect_.x_    = rect.value("x", crop_rect_.x_);
    crop_rect_.y_    = rect.value("y", crop_rect_.y_);
    crop_rect_.w_    = rect.value("w", crop_rect_.w_);
    crop_rect_.h_    = rect.value("h", crop_rect_.h_);
  }
  if (inner.contains("aspect_ratio_preset") && inner["aspect_ratio_preset"].is_string()) {
    aspect_ratio_preset_ = inner["aspect_ratio_preset"].get<std::string>();
  }
  if (inner.contains("aspect_ratio") && inner["aspect_ratio"].is_object()) {
    const auto& aspect   = inner["aspect_ratio"];
    aspect_ratio_width_  = aspect.value("width", aspect_ratio_width_);
    aspect_ratio_height_ = aspect.value("height", aspect_ratio_height_);
  }

  angle_degrees_ = NormalizeAngleDegrees(angle_degrees_);
  SanitizeAspectState(aspect_ratio_preset_, aspect_ratio_width_, aspect_ratio_height_);
  crop_rect_ = ClampCropRect(crop_rect_);
}

void CropRotateOp::SetGlobalParams(OperatorParams&) const {
  // Geometry operators are executed directly and do not populate stream global params.
}

void CropRotateOp::EnableGlobalParams(OperatorParams&, bool) {
  // Geometry operators are executed directly and do not populate stream global params.
}
}  // namespace alcedo
