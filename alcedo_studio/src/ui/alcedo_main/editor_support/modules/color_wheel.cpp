//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/editor_support/modules/color_wheel.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace alcedo::ui::color_wheel {
namespace {

auto HueToUnitRgb(float h) -> std::array<float, 3> {
  h                  = h - std::floor(h);
  const float scaled = h * 6.0f;
  const int   sector = static_cast<int>(std::floor(scaled)) % 6;
  const float f      = scaled - std::floor(scaled);
  switch (sector) {
    case 0:
      return {1.0f, f, 0.0f};
    case 1:
      return {1.0f - f, 1.0f, 0.0f};
    case 2:
      return {0.0f, 1.0f, f};
    case 3:
      return {0.0f, 1.0f - f, 1.0f};
    case 4:
      return {f, 0.0f, 1.0f};
    case 5:
    default:
      return {1.0f, 0.0f, 1.0f - f};
  }
}

}  // namespace

auto ClampDiscPoint(const QPointF& p) -> QPointF {
  const float x = static_cast<float>(p.x());
  const float y = static_cast<float>(p.y());
  if (!std::isfinite(x) || !std::isfinite(y)) {
    return QPointF(0.0, 0.0);
  }
  const float r = std::sqrt(x * x + y * y);
  if (r <= 1.0f || r <= kEpsilon) {
    return QPointF(x, y);
  }
  const float inv_r = 1.0f / r;
  return QPointF(x * inv_r, y * inv_r);
}

auto DiscToCdlDelta(const QPointF& position, float strength) -> std::array<float, 3> {
  const QPointF p = ClampDiscPoint(position);
  const float   x = static_cast<float>(p.x());
  const float   y = static_cast<float>(p.y());
  const float   r = std::clamp(std::sqrt(x * x + y * y), 0.0f, 1.0f);
  float         h = std::atan2(y, x) / (2.0f * std::numbers::pi_v<float>);
  if (h < 0.0f) {
    h += 1.0f;
  }
  const auto  c   = HueToUnitRgb(h);
  const float m   = (c[0] + c[1] + c[2]) / 3.0f;
  float       dr  = c[0] - m;
  float       dg  = c[1] - m;
  float       db  = c[2] - m;
  const float len = std::sqrt(dr * dr + dg * dg + db * db);
  const float inv = 1.0f / (len + kEpsilon);
  dr *= inv;
  dg *= inv;
  db *= inv;
  const float scale = std::max(strength, 0.0f) * r;
  return {dr * scale, dg * scale, db * scale};
}

auto CdlSliderUiToMaster(int slider_value) -> float {
  return std::clamp(static_cast<float>(slider_value) / kSliderToParam, -1.0f, 1.0f);
}

auto CdlMasterToSliderUi(float master_value) -> int {
  const float clamped = std::clamp(master_value, -1.0f, 1.0f);
  return std::clamp(static_cast<int>(std::lround(clamped * kSliderToParam)), kSliderUiMin,
                    kSliderUiMax);
}

}  // namespace alcedo::ui::color_wheel
