//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>

// Qt-free color wheel scale and disc equations. The editor parameter catalog and the CDL
// trackball model read the same values.
namespace alcedo::ui::color_wheel {

constexpr int   kSliderUiMin     = -800;
constexpr int   kSliderUiMax     = 800;
constexpr float kSliderToParam   = 8000.0f;
constexpr float kStrengthDefault = 0.10f;
constexpr float kEpsilon         = 1e-6f;

/// The disc point (@p x, @p y) moved onto the unit circle when it is outside. A point that is not
/// finite is the center.
auto            ClampDiscPoint(float x, float y) -> std::array<float, 2>;
/// RGB color offset of the disc point (@p x, @p y) at @p strength.
auto            DiscToCdlDelta(float x, float y, float strength) -> std::array<float, 3>;
auto            CdlSliderUiToMaster(int slider_value) -> float;
auto            CdlMasterToSliderUi(float master_value) -> int;

}  // namespace alcedo::ui::color_wheel
