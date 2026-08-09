//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QColor>
#include <array>

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

using HlsProfileArray                                 = std::array<float, kCandidateHues.size()>;

inline auto MakeFilledArray(float value) -> HlsProfileArray {
  HlsProfileArray out{};
  out.fill(value);
  return out;
}

auto WrapHueDegrees(float hue) -> float;
auto HueDistanceDegrees(float a, float b) -> float;
auto ClosestCandidateHueIndex(float hue) -> int;
auto CandidateColor(float hue_degrees) -> QColor;

}  // namespace alcedo::ui::hls
