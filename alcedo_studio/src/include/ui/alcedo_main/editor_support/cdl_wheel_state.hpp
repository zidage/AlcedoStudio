//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QPointF>
#include <QString>
#include <array>

#include "ui/alcedo_main/editor_support/modules/color_wheel.hpp"

namespace alcedo::ui {

/// Panel state of one tone wheel. Every value except the disc position is in stops.
struct CdlWheelState {
  QPointF              disc_position_ = QPointF(0.0, 0.0);
  float                master_offset_ = 0.0f;
  std::array<float, 3> color_offset_  = {0.0f, 0.0f, 0.0f};
  float                strength_      = color_wheel::kDiscMaxStops;
};

/// Per-channel change in stops of @p wheel: the disc stops plus the Master value.
inline auto DisplayWheelDelta(const CdlWheelState& wheel) -> std::array<float, 3> {
  const float master = wheel.master_offset_;
  return {wheel.color_offset_[0] + master, wheel.color_offset_[1] + master,
          wheel.color_offset_[2] + master};
}

inline auto FormatSignedStops(float value) -> QString {
  if (value >= 0.0f) {
    return QString("+%1").arg(value, 0, 'f', 2);
  }
  return QString::number(value, 'f', 2);
}

inline auto FormatWheelDeltaText(const CdlWheelState& wheel) -> QString {
  const auto delta = DisplayWheelDelta(wheel);
  return QString("R %1  G %2  B %3 EV")
      .arg(FormatSignedStops(delta[0]), FormatSignedStops(delta[1]), FormatSignedStops(delta[2]));
}

}  // namespace alcedo::ui
