//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/mask/analytic_mask_edit.hpp"

#include <algorithm>
#include <cmath>
#include <variant>

namespace alcedo {
namespace {

constexpr float kPi = 3.14159265358979323846f;

[[nodiscard]] auto IsFinite(Vector2 point) -> bool {
  return std::isfinite(point.x) && std::isfinite(point.y);
}

[[nodiscard]] auto IsFiniteRadial(const RadialMaskSource& source) -> bool {
  return std::isfinite(source.center_x) && std::isfinite(source.center_y) &&
         std::isfinite(source.major_radius) && std::isfinite(source.minor_radius) &&
         std::isfinite(source.rotation) && std::isfinite(source.inner_feather) &&
         std::isfinite(source.outer_feather);
}

[[nodiscard]] auto IsFiniteLinear(const LinearGradientMaskSource& source) -> bool {
  return std::isfinite(source.origin_x) && std::isfinite(source.origin_y) &&
         std::isfinite(source.normal_x) && std::isfinite(source.normal_y) &&
         std::isfinite(source.transition_distance) && std::isfinite(source.start_value) &&
         std::isfinite(source.end_value);
}

[[nodiscard]] auto LinearNormalLength(const LinearGradientMaskSource& source) -> float {
  return std::hypot(source.normal_x, source.normal_y);
}

[[nodiscard]] auto UnwrapRadians(float previous, float candidate) -> float {
  float delta = candidate - previous;
  while (delta > kPi) {
    delta -= 2.0f * kPi;
  }
  while (delta < -kPi) {
    delta += 2.0f * kPi;
  }
  return previous + delta;
}

}  // namespace

auto RadialLocalAxes(const RadialMaskSource& source, Vector2 normalized) -> std::optional<Vector2> {
  if (!IsFiniteRadial(source) || !IsFinite(normalized)) {
    return std::nullopt;
  }
  const float c  = std::cos(source.rotation);
  const float s  = std::sin(source.rotation);
  const float dx = normalized.x - source.center_x;
  const float dy = normalized.y - source.center_y;
  if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(c) || !std::isfinite(s)) {
    return std::nullopt;
  }
  return Vector2{c * dx + s * dy, -s * dx + c * dy};
}

auto RadialRho(const RadialMaskSource& source, Vector2 normalized) -> std::optional<float> {
  const auto local = RadialLocalAxes(source, normalized);
  if (!local) {
    return std::nullopt;
  }
  const float u =
      local->x / std::max(source.major_radius, kAnalyticMaskEpsilon);
  const float v =
      local->y / std::max(source.minor_radius, kAnalyticMaskEpsilon);
  const float rho = std::sqrt(u * u + v * v);
  if (!std::isfinite(rho)) {
    return std::nullopt;
  }
  return rho;
}

auto RadialCreationIsValid(const RadialMaskSource& source) -> bool {
  return IsFiniteRadial(source) && source.major_radius > kAnalyticCreationMinRadius &&
         source.minor_radius > kAnalyticCreationMinRadius && source.major_radius >= 0.0f &&
         source.minor_radius >= 0.0f && source.inner_feather >= 0.0f &&
         source.outer_feather >= 0.0f;
}

auto LinearCreationIsValid(const LinearGradientMaskSource& source) -> bool {
  return IsFiniteLinear(source) && LinearNormalLength(source) > kAnalyticMaskEpsilon &&
         source.transition_distance >= 0.0f;
}

auto RadialFromCenterOut(Vector2 center_normalized, Vector2 current_normalized)
    -> RadialMaskSource {
  RadialMaskSource source;
  source.center_x     = center_normalized.x;
  source.center_y     = center_normalized.y;
  source.major_radius = std::fabs(current_normalized.x - center_normalized.x);
  source.minor_radius = std::fabs(current_normalized.y - center_normalized.y);
  source.rotation     = 0.0f;
  source.inner_feather = 0.0f;
  source.outer_feather = 0.0f;
  return source;
}

auto LinearFromEndpoints(Vector2 a_normalized, Vector2 b_normalized)
    -> LinearGradientMaskSource {
  LinearGradientMaskSource source;
  source.origin_x            = 0.5f * (a_normalized.x + b_normalized.x);
  source.origin_y            = 0.5f * (a_normalized.y + b_normalized.y);
  const float dx             = b_normalized.x - a_normalized.x;
  const float dy             = b_normalized.y - a_normalized.y;
  const float length         = std::hypot(dx, dy);
  source.transition_distance = length;
  source.start_value         = 1.0f;
  source.end_value           = 0.0f;
  if (length > kAnalyticMaskEpsilon && std::isfinite(length)) {
    source.normal_x = dx / length;
    source.normal_y = dy / length;
  } else {
    source.normal_x = 0.0f;
    source.normal_y = 0.0f;
  }
  return source;
}

auto TranslateRadialCenter(RadialMaskSource source, Vector2 center_normalized)
    -> RadialMaskSource {
  source.center_x = center_normalized.x;
  source.center_y = center_normalized.y;
  return source;
}

auto TranslateLinearOrigin(LinearGradientMaskSource source, Vector2 origin_normalized)
    -> LinearGradientMaskSource {
  source.origin_x = origin_normalized.x;
  source.origin_y = origin_normalized.y;
  return source;
}

auto UpdateRadialMajorRadius(const RadialMaskSource& source, Vector2 normalized)
    -> std::optional<RadialMaskSource> {
  const auto local = RadialLocalAxes(source, normalized);
  if (!local) {
    return std::nullopt;
  }
  RadialMaskSource next = source;
  next.major_radius     = std::fabs(local->x);
  return next;
}

auto UpdateRadialMinorRadius(const RadialMaskSource& source, Vector2 normalized)
    -> std::optional<RadialMaskSource> {
  const auto local = RadialLocalAxes(source, normalized);
  if (!local) {
    return std::nullopt;
  }
  RadialMaskSource next = source;
  next.minor_radius     = std::fabs(local->y);
  return next;
}

auto UpdateRadialRotation(const RadialMaskSource& source, Vector2 normalized,
                          float& unwrapped_rotation) -> std::optional<RadialMaskSource> {
  if (!IsFiniteRadial(source) || !IsFinite(normalized)) {
    return std::nullopt;
  }
  const float dx = normalized.x - source.center_x;
  const float dy = normalized.y - source.center_y;
  if (!std::isfinite(dx) || !std::isfinite(dy) || std::hypot(dx, dy) <= kAnalyticMaskEpsilon) {
    return std::nullopt;
  }
  const float wrapped = std::atan2(dy, dx);
  if (!std::isfinite(wrapped)) {
    return std::nullopt;
  }
  unwrapped_rotation        = UnwrapRadians(unwrapped_rotation, wrapped);
  RadialMaskSource next     = source;
  next.rotation             = unwrapped_rotation;
  return next;
}

auto UpdateRadialInnerFeather(const RadialMaskSource& source, Vector2 normalized)
    -> std::optional<RadialMaskSource> {
  const auto rho = RadialRho(source, normalized);
  if (!rho) {
    return std::nullopt;
  }
  RadialMaskSource next = source;
  next.inner_feather    = std::clamp(1.0f - *rho, 0.0f, 1.0f);
  return next;
}

auto UpdateRadialOuterFeather(const RadialMaskSource& source, Vector2 normalized)
    -> std::optional<RadialMaskSource> {
  const auto rho = RadialRho(source, normalized);
  if (!rho) {
    return std::nullopt;
  }
  RadialMaskSource next = source;
  next.outer_feather    = std::max(0.0f, *rho - 1.0f);
  return next;
}

auto UpdateLinearDirection(const LinearGradientMaskSource& source, Vector2 normalized)
    -> std::optional<LinearGradientMaskSource> {
  if (!IsFiniteLinear(source) || !IsFinite(normalized)) {
    return std::nullopt;
  }
  const float dx     = normalized.x - source.origin_x;
  const float dy     = normalized.y - source.origin_y;
  const float length = std::hypot(dx, dy);
  if (!std::isfinite(length) || length <= kAnalyticMaskEpsilon) {
    return std::nullopt;
  }
  LinearGradientMaskSource next = source;
  next.normal_x                 = dx / length;
  next.normal_y                 = dy / length;
  return next;
}

auto UpdateLinearTransitionFromBoundary(const LinearGradientMaskSource& source,
                                        Vector2 normalized)
    -> std::optional<LinearGradientMaskSource> {
  if (!IsFiniteLinear(source) || !IsFinite(normalized)) {
    return std::nullopt;
  }
  const float length = LinearNormalLength(source);
  if (length <= kAnalyticMaskEpsilon) {
    return std::nullopt;
  }
  const float nx = source.normal_x / length;
  const float ny = source.normal_y / length;
  const float d  = (normalized.x - source.origin_x) * nx + (normalized.y - source.origin_y) * ny;
  if (!std::isfinite(d)) {
    return std::nullopt;
  }
  LinearGradientMaskSource next = source;
  next.transition_distance      = 2.0f * std::fabs(d);
  return next;
}

auto ApplyAnalyticMaskHandle(AnalyticMaskHandle handle, const MaskSource& source,
                             Vector2 normalized, float& unwrapped_rotation)
    -> std::optional<MaskSource> {
  if (handle == AnalyticMaskHandle::None) {
    return std::nullopt;
  }
  if (const auto* radial = std::get_if<RadialMaskSource>(&source)) {
    switch (handle) {
      case AnalyticMaskHandle::RadialCenter:
        if (!IsFinite(normalized)) {
          return std::nullopt;
        }
        return TranslateRadialCenter(*radial, normalized);
      case AnalyticMaskHandle::RadialMajor:
        if (const auto next = UpdateRadialMajorRadius(*radial, normalized)) {
          return *next;
        }
        return std::nullopt;
      case AnalyticMaskHandle::RadialMinor:
        if (const auto next = UpdateRadialMinorRadius(*radial, normalized)) {
          return *next;
        }
        return std::nullopt;
      case AnalyticMaskHandle::RadialRotate:
        if (const auto next = UpdateRadialRotation(*radial, normalized, unwrapped_rotation)) {
          return *next;
        }
        return std::nullopt;
      case AnalyticMaskHandle::RadialInnerFeather:
        if (const auto next = UpdateRadialInnerFeather(*radial, normalized)) {
          return *next;
        }
        return std::nullopt;
      case AnalyticMaskHandle::RadialOuterFeather:
        if (const auto next = UpdateRadialOuterFeather(*radial, normalized)) {
          return *next;
        }
        return std::nullopt;
      default:
        return std::nullopt;
    }
  }
  if (const auto* linear = std::get_if<LinearGradientMaskSource>(&source)) {
    switch (handle) {
      case AnalyticMaskHandle::LinearOrigin:
        if (!IsFinite(normalized)) {
          return std::nullopt;
        }
        return TranslateLinearOrigin(*linear, normalized);
      case AnalyticMaskHandle::LinearDirection:
        if (const auto next = UpdateLinearDirection(*linear, normalized)) {
          return *next;
        }
        return std::nullopt;
      case AnalyticMaskHandle::LinearStartBoundary:
      case AnalyticMaskHandle::LinearEndBoundary:
        if (const auto next = UpdateLinearTransitionFromBoundary(*linear, normalized)) {
          return *next;
        }
        return std::nullopt;
      default:
        return std::nullopt;
    }
  }
  return std::nullopt;
}

}  // namespace alcedo
