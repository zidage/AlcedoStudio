//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <optional>
#include <string_view>

namespace alcedo::ui::geometry {

constexpr float kRotationSliderScale = 100.0f;
constexpr float kCropRectSliderScale = 1000.0f;
constexpr float kCropRectMinSize     = 1e-4f;
constexpr float kCropAspectMinValue  = 1e-4f;

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

struct CropAspectPresetOption {
  CropAspectPreset value_;
  const char*      id_;
  const char*      label_;
  float            width_;
  float            height_;
};

struct ResolvedCropRect {
  float x_            = 0.0f;
  float y_            = 0.0f;
  float w_            = 1.0f;
  float h_            = 1.0f;
  float aspect_ratio_ = 1.0f;
  int   pixel_width_  = 0;
  int   pixel_height_ = 0;
};

auto ClampCropRect(float x, float y, float w, float h) -> std::array<float, 4>;
auto NormalizeCropAspect(float width, float height) -> std::optional<std::array<float, 2>>;
auto HasLockedAspect(CropAspectPreset preset, float width, float height) -> bool;
auto AspectRatioFromSize(float width, float height) -> std::optional<float>;
auto CropAspectPresetOptions() -> const std::array<CropAspectPresetOption, 12>&;
auto CropAspectPresetToString(CropAspectPreset preset) -> std::string_view;
auto ParseCropAspectPreset(std::string_view preset) -> std::optional<CropAspectPreset>;
auto CropAspectPresetLabel(CropAspectPreset preset) -> const char*;
auto CropAspectPresetRatio(CropAspectPreset preset) -> std::optional<std::array<float, 2>>;
auto MakeMaxAspectCropRect(float image_aspect, float aspect_ratio) -> std::array<float, 4>;
auto FitAspectRectInsideBounds(float x, float y, float w, float h, float image_aspect,
                               float aspect_ratio) -> std::array<float, 4>;
auto ResizeAspectRectAroundCenter(float x, float y, float w, float h, float image_aspect,
                                  float aspect_ratio, bool use_width_driver)
    -> std::array<float, 4>;
auto ResolveCropRect(float x, float y, float w, float h, float image_aspect,
                     CropAspectPreset preset, float aspect_width, float aspect_height,
                     int source_width = 0, int source_height = 0) -> ResolvedCropRect;

}  // namespace alcedo::ui::geometry
