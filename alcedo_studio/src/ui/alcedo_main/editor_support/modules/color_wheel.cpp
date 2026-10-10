//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/editor_support/modules/color_wheel.hpp"

namespace alcedo::ui::color_wheel {

auto ClampDiscPoint(const QPointF& p) -> QPointF {
  const auto clamped = ClampDiscPoint(static_cast<float>(p.x()), static_cast<float>(p.y()));
  return QPointF(clamped[0], clamped[1]);
}

auto DiscToCdlDelta(const QPointF& position, float strength) -> std::array<float, 3> {
  return DiscToCdlDelta(static_cast<float>(position.x()), static_cast<float>(position.y()),
                        strength);
}

}  // namespace alcedo::ui::color_wheel
