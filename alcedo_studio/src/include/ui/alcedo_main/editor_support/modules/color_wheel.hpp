//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QPointF>
#include <array>

namespace alcedo::ui::color_wheel {

constexpr int   kSliderUiMin     = -800;
constexpr int   kSliderUiMax     = 800;
constexpr float kSliderToParam   = 8000.0f;
constexpr float kStrengthDefault = 0.10f;
constexpr float kEpsilon         = 1e-6f;

auto            ClampDiscPoint(const QPointF& p) -> QPointF;
auto            DiscToCdlDelta(const QPointF& position, float strength) -> std::array<float, 3>;
auto            CdlSliderUiToMaster(int slider_value) -> float;
auto            CdlMasterToSliderUi(float master_value) -> int;

}  // namespace alcedo::ui::color_wheel
