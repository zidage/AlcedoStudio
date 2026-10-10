//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QPointF>
#include <array>

#include "ui/alcedo_main/editor_support/modules/color_wheel_math.hpp"

namespace alcedo::ui::color_wheel {

auto ClampDiscPoint(const QPointF& p) -> QPointF;
auto DiscToCdlDelta(const QPointF& position, float strength) -> std::array<float, 3>;

}  // namespace alcedo::ui::color_wheel
