//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/editor_support/modules/hls.hpp"

namespace alcedo::ui::hls {

auto CandidateColor(float hue_degrees) -> QColor {
  const float wrapped = WrapHueDegrees(hue_degrees);
  return QColor::fromHslF(wrapped / 360.0f, 1.0f, 0.5f);
}

}  // namespace alcedo::ui::hls
