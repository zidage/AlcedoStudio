//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/editor_support/modules/color_temp.hpp"

#include <algorithm>
#include <cmath>

namespace alcedo::ui::color_temp {

auto SliderPosToCct(int pos) -> float {
  const float min_cct     = static_cast<float>(kCctMin);
  const float max_cct     = static_cast<float>(kCctMax);
  const int   clamped_pos = std::clamp(pos, kSliderUiMin, kSliderUiMax);

  float       cct         = min_cct;
  if (clamped_pos <= kSliderUiMid) {
    const float t = static_cast<float>(clamped_pos - kSliderUiMin) /
                    static_cast<float>(kSliderUiMid - kSliderUiMin);
    cct = min_cct + t * (kPivotCct - min_cct);
  } else {
    const float t = static_cast<float>(clamped_pos - kSliderUiMid) /
                    static_cast<float>(kSliderUiMax - kSliderUiMid);
    cct = kPivotCct + t * (max_cct - kPivotCct);
  }

  return std::clamp(cct, min_cct, max_cct);
}

auto CctToSliderPos(float cct) -> int {
  const float min_cct     = static_cast<float>(kCctMin);
  const float max_cct     = static_cast<float>(kCctMax);
  const float clamped_cct = std::clamp(cct, min_cct, max_cct);

  float       pos         = static_cast<float>(kSliderUiMin);
  if (clamped_cct <= kPivotCct) {
    const float t = (clamped_cct - min_cct) / (kPivotCct - min_cct);
    pos = static_cast<float>(kSliderUiMin) + t * static_cast<float>(kSliderUiMid - kSliderUiMin);
  } else {
    const float t = (clamped_cct - kPivotCct) / (max_cct - kPivotCct);
    pos = static_cast<float>(kSliderUiMid) + t * static_cast<float>(kSliderUiMax - kSliderUiMid);
  }

  return std::clamp(static_cast<int>(std::lround(pos)), kSliderUiMin, kSliderUiMax);
}

}  // namespace alcedo::ui::color_temp
