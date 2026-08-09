//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/editor_support/modules/geometry.hpp"

#include <algorithm>
#include <cmath>

namespace alcedo::ui::geometry {
namespace {

constexpr float                                  kAspectEpsilon           = 1e-4f;

constexpr std::array<CropAspectPresetOption, 12> kCropAspectPresetOptions = {{
    {CropAspectPreset::Free, "free", "Free", 0.0f, 0.0f},
    {CropAspectPreset::Custom, "custom", "Custom", 0.0f, 0.0f},
    {CropAspectPreset::Ratio235_1_35mm, "ratio_2_35_1_35mm", "2.35:1 (35mm)", 2.35f, 1.0f},
    {CropAspectPreset::Ratio1_1, "ratio_1_1", "1:1", 1.0f, 1.0f},
    {CropAspectPreset::Ratio16_9, "ratio_16_9", "16:9", 16.0f, 9.0f},
    {CropAspectPreset::Ratio1_9_IMAX, "ratio_1_9_1_imax", "1.9:1 (IMAX)", 1.9f, 1.0f},
    {CropAspectPreset::Ratio1_85_DCI, "ratio_1_85_1_dci", "1.85:1 (DCI)", 1.85f, 1.0f},
    {CropAspectPreset::Ratio2_2_70mm, "ratio_2_2_1_70mm", "2.2:1 (70mm)", 2.2f, 1.0f},
    {CropAspectPreset::Ratio1_43_70mm_IMAX, "ratio_1_43_1_70mm_imax", "1.43:1 (70mm IMAX)", 1.43f,
     1.0f},
    {CropAspectPreset::Ratio4_3_35mm, "ratio_4_3_35mm", "4:3 (35mm)", 4.0f, 3.0f},
    {CropAspectPreset::Ratio1_5_NativeOrVistaVision, "ratio_1_5_1_native_vistavision",
     "1.5:1 (native or VistaVision)", 1.5f, 1.0f},
    {CropAspectPreset::Ratio2_76_PanavisionUltra, "ratio_2_76_1_panavision_ultra",
     "2.76:1 (Panavision Ultra)", 2.76f, 1.0f},
}};

auto                                             ClampImageAspect(float image_aspect) -> float {
  return std::max(image_aspect, kCropAspectMinValue);
}

auto ResolveAspectRatio(CropAspectPreset preset, float width, float height)
    -> std::optional<float> {
  if (preset == CropAspectPreset::Free) {
    return std::nullopt;
  }
  if (const auto preset_ratio = CropAspectPresetRatio(preset); preset_ratio.has_value()) {
    return preset_ratio->at(0) / std::max(preset_ratio->at(1), kCropAspectMinValue);
  }
  return AspectRatioFromSize(width, height);
}

auto ClampPixelExtent(int extent, int maximum) -> int {
  if (maximum <= 0) {
    return 0;
  }
  return std::clamp(extent, 1, maximum);
}

}  // namespace

auto ClampCropRect(float x, float y, float w, float h) -> std::array<float, 4> {
  w = std::clamp(w, kCropRectMinSize, 1.0f);
  h = std::clamp(h, kCropRectMinSize, 1.0f);
  x = std::clamp(x, 0.0f, 1.0f - w);
  y = std::clamp(y, 0.0f, 1.0f - h);
  return {x, y, w, h};
}

auto NormalizeCropAspect(float width, float height) -> std::optional<std::array<float, 2>> {
  if (!std::isfinite(width) || !std::isfinite(height) || width <= kCropAspectMinValue ||
      height <= kCropAspectMinValue) {
    return std::nullopt;
  }
  return std::array<float, 2>{
      std::max(width, kCropAspectMinValue),
      std::max(height, kCropAspectMinValue),
  };
}

auto HasLockedAspect(CropAspectPreset preset, float width, float height) -> bool {
  if (preset == CropAspectPreset::Free) {
    return false;
  }
  if (preset != CropAspectPreset::Custom) {
    return CropAspectPresetRatio(preset).has_value();
  }
  return NormalizeCropAspect(width, height).has_value();
}

auto AspectRatioFromSize(float width, float height) -> std::optional<float> {
  const auto normalized = NormalizeCropAspect(width, height);
  if (!normalized.has_value()) {
    return std::nullopt;
  }
  return normalized->at(0) / std::max(normalized->at(1), kCropAspectMinValue);
}

auto CropAspectPresetOptions() -> const std::array<CropAspectPresetOption, 12>& {
  return kCropAspectPresetOptions;
}

auto CropAspectPresetToString(CropAspectPreset preset) -> std::string_view {
  for (const auto& option : kCropAspectPresetOptions) {
    if (option.value_ == preset) {
      return option.id_;
    }
  }
  return "free";
}

auto ParseCropAspectPreset(std::string_view preset) -> std::optional<CropAspectPreset> {
  for (const auto& option : kCropAspectPresetOptions) {
    if (preset == option.id_) {
      return option.value_;
    }
  }
  return std::nullopt;
}

auto CropAspectPresetLabel(CropAspectPreset preset) -> const char* {
  for (const auto& option : kCropAspectPresetOptions) {
    if (option.value_ == preset) {
      return option.label_;
    }
  }
  return "Free";
}

auto CropAspectPresetRatio(CropAspectPreset preset) -> std::optional<std::array<float, 2>> {
  for (const auto& option : kCropAspectPresetOptions) {
    if (option.value_ != preset) {
      continue;
    }
    return NormalizeCropAspect(option.width_, option.height_);
  }
  return std::nullopt;
}

auto MakeMaxAspectCropRect(float image_aspect, float aspect_ratio) -> std::array<float, 4> {
  return FitAspectRectInsideBounds(0.0f, 0.0f, 1.0f, 1.0f, image_aspect, aspect_ratio);
}

auto FitAspectRectInsideBounds(float x, float y, float w, float h, float image_aspect,
                               float aspect_ratio) -> std::array<float, 4> {
  const auto  clamped               = ClampCropRect(x, y, w, h);
  const float ratio                 = std::max(aspect_ratio, kCropAspectMinValue);
  const float source_aspect         = ClampImageAspect(image_aspect);
  const float max_width_from_height = clamped[3] * (ratio / source_aspect);
  const bool  width_limited         = max_width_from_height <= clamped[2] + kAspectEpsilon;

  float       resolved_w            = clamped[2];
  float       resolved_h            = clamped[3];
  if (width_limited) {
    resolved_w = std::clamp(max_width_from_height, kCropRectMinSize, clamped[2]);
    resolved_h = std::clamp(resolved_w * (source_aspect / ratio), kCropRectMinSize, clamped[3]);
  } else {
    resolved_h = std::clamp(clamped[2] * (source_aspect / ratio), kCropRectMinSize, clamped[3]);
    resolved_w = std::clamp(resolved_h * (ratio / source_aspect), kCropRectMinSize, clamped[2]);
  }

  const float cx = clamped[0] + (clamped[2] * 0.5f);
  const float cy = clamped[1] + (clamped[3] * 0.5f);
  return ClampCropRect(cx - (resolved_w * 0.5f), cy - (resolved_h * 0.5f), resolved_w, resolved_h);
}

auto ResizeAspectRectAroundCenter(float x, float y, float w, float h, float image_aspect,
                                  float aspect_ratio, bool use_width_driver)
    -> std::array<float, 4> {
  const auto  clamped    = ClampCropRect(x, y, w, h);
  const float ratio      = std::max(aspect_ratio, kCropAspectMinValue);
  const float img_aspect = ClampImageAspect(image_aspect);

  float       resolved_w = clamped[2];
  float       resolved_h = clamped[3];
  if (use_width_driver) {
    resolved_h = std::max(kCropRectMinSize, resolved_w * (img_aspect / ratio));
    if (resolved_h > 1.0f) {
      const float scale = 1.0f / resolved_h;
      resolved_h *= scale;
      resolved_w *= scale;
    }
  } else {
    resolved_w = std::max(kCropRectMinSize, resolved_h * (ratio / img_aspect));
    if (resolved_w > 1.0f) {
      const float scale = 1.0f / resolved_w;
      resolved_w *= scale;
      resolved_h *= scale;
    }
  }

  const float cx = clamped[0] + (clamped[2] * 0.5f);
  const float cy = clamped[1] + (clamped[3] * 0.5f);
  return ClampCropRect(cx - (resolved_w * 0.5f), cy - (resolved_h * 0.5f), resolved_w, resolved_h);
}

auto ResolveCropRect(float x, float y, float w, float h, float image_aspect,
                     CropAspectPreset preset, float aspect_width, float aspect_height,
                     int source_width, int source_height) -> ResolvedCropRect {
  const auto clamped      = ClampCropRect(x, y, w, h);
  const auto aspect_ratio = ResolveAspectRatio(preset, aspect_width, aspect_height);
  const auto resolved_rect =
      aspect_ratio.has_value() ? FitAspectRectInsideBounds(clamped[0], clamped[1], clamped[2],
                                                           clamped[3], image_aspect, *aspect_ratio)
                               : clamped;
  const float resolved_ratio =
      aspect_ratio.value_or((resolved_rect[2] * ClampImageAspect(image_aspect)) /
                            std::max(resolved_rect[3], kCropRectMinSize));

  ResolvedCropRect resolved;
  resolved.x_            = resolved_rect[0];
  resolved.y_            = resolved_rect[1];
  resolved.w_            = resolved_rect[2];
  resolved.h_            = resolved_rect[3];
  resolved.aspect_ratio_ = resolved_ratio;

  if (source_width > 0 && source_height > 0) {
    resolved.pixel_width_ =
        ClampPixelExtent(static_cast<int>(std::lround(source_width * resolved.w_)), source_width);
    resolved.pixel_height_ =
        ClampPixelExtent(static_cast<int>(std::lround(source_height * resolved.h_)), source_height);
  }

  return resolved;
}

}  // namespace alcedo::ui::geometry
