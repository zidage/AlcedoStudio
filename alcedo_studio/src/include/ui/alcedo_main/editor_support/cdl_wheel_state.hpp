//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QPointF>
#include <QString>
#include <array>

#include "ui/alcedo_main/editor_support/modules/color_wheel.hpp"

namespace alcedo::ui {

struct CdlWheelState {
  QPointF              disc_position_ = QPointF(0.0, 0.0);
  float                master_offset_ = 0.0f;
  std::array<float, 3> color_offset_  = {0.0f, 0.0f, 0.0f};
  float                strength_      = color_wheel::kStrengthDefault;
};

inline auto DefaultLiftWheelState() -> CdlWheelState { return {}; }

inline auto DefaultGammaGainWheelState() -> CdlWheelState {
  CdlWheelState wheel;
  wheel.color_offset_ = {1.0f, 1.0f, 1.0f};
  return wheel;
}

inline auto DisplayWheelDelta(const CdlWheelState& wheel, bool add_unity) -> std::array<float, 3> {
  const float master = wheel.master_offset_;
  if (add_unity) {
    return {wheel.color_offset_[0] - 1.0f + master, wheel.color_offset_[1] - 1.0f + master,
            wheel.color_offset_[2] - 1.0f + master};
  }
  return {wheel.color_offset_[0] + master, wheel.color_offset_[1] + master,
          wheel.color_offset_[2] + master};
}

inline auto FormatSigned3(float value) -> QString {
  if (value >= 0.0f) {
    return QString("+%1").arg(value, 0, 'f', 3);
  }
  return QString::number(value, 'f', 3);
}

inline auto FormatWheelDeltaText(const CdlWheelState& wheel, bool add_unity) -> QString {
  const auto delta = DisplayWheelDelta(wheel, add_unity);
  return QString("R %1  G %2  B %3")
      .arg(FormatSigned3(delta[0]), FormatSigned3(delta[1]), FormatSigned3(delta[2]));
}

}  // namespace alcedo::ui
