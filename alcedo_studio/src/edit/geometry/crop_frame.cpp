//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/geometry/crop_frame.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace alcedo {
namespace {

constexpr float kPi = 3.14159265358979323846f;

}  // namespace

auto NormalizeRotationDegrees(float degrees) -> float {
  if (!std::isfinite(degrees)) {
    return 0.0f;
  }
  degrees = std::fmod(degrees, 360.0f);
  if (degrees > 180.0f) {
    degrees -= 360.0f;
  } else if (degrees < -180.0f) {
    degrees += 360.0f;
  }
  return degrees;
}

auto ClampCropToRotatedSource(NormalizedRect crop, float rotation_degrees, Extent2D source)
    -> NormalizedRect {
  if (!std::isfinite(crop.x) || !std::isfinite(crop.y) || !std::isfinite(crop.w) ||
      !std::isfinite(crop.h)) {
    throw std::runtime_error("ClampCropToRotatedSource: crop_rect must be finite");
  }
  if (source.Empty()) {
    throw std::runtime_error("ClampCropToRotatedSource: source extent must be positive");
  }
  const float full_w      = static_cast<float>(source.width);
  const float full_h      = static_cast<float>(source.height);
  const float min_half_w  = 0.5f * kGeometryMinNormalizedSize * full_w;
  const float min_half_h  = 0.5f * kGeometryMinNormalizedSize * full_h;

  const float w           = std::clamp(crop.w, kGeometryMinNormalizedSize, 1.0f);
  const float h           = std::clamp(crop.h, kGeometryMinNormalizedSize, 1.0f);
  float       center_x    = std::clamp(crop.x + 0.5f * crop.w, 0.0f, 1.0f) * full_w;
  float       center_y    = std::clamp(crop.y + 0.5f * crop.h, 0.0f, 1.0f) * full_h;
  float       half_w      = 0.5f * w * full_w;
  float       half_h      = 0.5f * h * full_h;

  const float theta       = NormalizeRotationDegrees(rotation_degrees) * (kPi / 180.0f);
  const float cosine      = std::fabs(std::cos(theta));
  const float sine        = std::fabs(std::sin(theta));
  const auto  reach_x     = [&] { return cosine * half_w + sine * half_h; };
  const auto  reach_y     = [&] { return sine * half_w + cosine * half_h; };

  const float limit_x     = 0.5f * full_w;
  const float limit_y     = 0.5f * full_h;
  if (reach_x() > limit_x || reach_y() > limit_y) {
    const float scale = std::min(limit_x / reach_x(), limit_y / reach_y());
    half_w            = std::max(min_half_w, half_w * scale);
    half_h            = std::max(min_half_h, half_h * scale);
  }
  const float extent_x = std::min(reach_x(), limit_x);
  const float extent_y = std::min(reach_y(), limit_y);
  center_x             = std::clamp(center_x, extent_x, full_w - extent_x);
  center_y             = std::clamp(center_y, extent_y, full_h - extent_y);

  NormalizedRect result;
  result.w = (2.0f * half_w) / full_w;
  result.h = (2.0f * half_h) / full_h;
  result.x = center_x / full_w - 0.5f * result.w;
  result.y = center_y / full_h - 0.5f * result.h;
  return result;
}

auto CropFrameCornersInReference(NormalizedRect crop, float rotation_degrees, Extent2D source)
    -> std::array<Vector2, 4> {
  const float full_w   = static_cast<float>(source.width);
  const float full_h   = static_cast<float>(source.height);
  const float center_x = (crop.x + 0.5f * crop.w) * full_w;
  const float center_y = (crop.y + 0.5f * crop.h) * full_h;
  const float half_w   = 0.5f * crop.w * full_w;
  const float half_h   = 0.5f * crop.h * full_h;
  // ResolveRenderGeometry maps reference -> output with Rotate(theta) about the center, so an
  // output offset maps back to the reference with Rotate(-theta).
  const auto  output_to_reference =
      Matrix3x3::Translate(center_x, center_y) *
      Matrix3x3::Rotate(-NormalizeRotationDegrees(rotation_degrees) * (kPi / 180.0f));
  return {TransformPoint(output_to_reference, Vector2{-half_w, -half_h}),
          TransformPoint(output_to_reference, Vector2{half_w, -half_h}),
          TransformPoint(output_to_reference, Vector2{half_w, half_h}),
          TransformPoint(output_to_reference, Vector2{-half_w, half_h})};
}

}  // namespace alcedo
