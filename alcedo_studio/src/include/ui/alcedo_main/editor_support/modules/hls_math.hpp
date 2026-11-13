//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>

// Qt-free HLS panel scale and hue equations. The editor parameter catalog and the QML HLS model
// read the same values.
namespace alcedo::ui::hls {

constexpr std::array<float, 8> kCandidateHues         = {0.0f,   45.0f,  90.0f,  135.0f,
                                                         180.0f, 225.0f, 270.0f, 315.0f};
constexpr float                kFixedTargetLightness  = 0.5f;
constexpr float                kFixedTargetSaturation = 0.5f;
constexpr float                kDefaultHueRange       = 45.0f;
constexpr float                kFixedLightnessRange   = 1.0f;
constexpr float                kFixedSaturationRange  = 1.0f;
constexpr float                kMaxHueShiftDegrees    = 30.0f;
constexpr float                kAdjUiMin              = -100.0f;
constexpr float                kAdjUiMax              = 100.0f;
constexpr float                kAdjUiToParamScale     = 1000.0f;
/// Hue Smoothness slider range in degrees: the hue range of one bin.
constexpr float                kHueRangeUiMin         = 1.0f;
constexpr float                kHueRangeUiMax         = 180.0f;

using HlsProfileArray                                 = std::array<float, kCandidateHues.size()>;

inline auto MakeFilledArray(float value) -> HlsProfileArray {
  HlsProfileArray out{};
  out.fill(value);
  return out;
}

auto WrapHueDegrees(float hue) -> float;
auto HueDistanceDegrees(float a, float b) -> float;
auto ClosestCandidateHueIndex(float hue) -> int;

}  // namespace alcedo::ui::hls
