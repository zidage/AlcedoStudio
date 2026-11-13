//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/editor_support/modules/hls_math.hpp"

#include <algorithm>
#include <cmath>

namespace alcedo::ui::hls {

auto WrapHueDegrees(float hue) -> float {
  hue = std::fmod(hue, 360.0f);
  if (hue < 0.0f) {
    hue += 360.0f;
  }
  return hue;
}

auto HueDistanceDegrees(float a, float b) -> float {
  const float diff = std::abs(WrapHueDegrees(a) - WrapHueDegrees(b));
  return std::min(diff, 360.0f - diff);
}

auto ClosestCandidateHueIndex(float hue) -> int {
  int   best_idx  = 0;
  float best_dist = HueDistanceDegrees(hue, kCandidateHues.front());
  for (int i = 1; i < static_cast<int>(kCandidateHues.size()); ++i) {
    const float dist = HueDistanceDegrees(hue, kCandidateHues[i]);
    if (dist < best_dist) {
      best_dist = dist;
      best_idx  = i;
    }
  }
  return best_idx;
}

}  // namespace alcedo::ui::hls
