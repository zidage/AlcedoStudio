//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/edit_viewer/crop_geometry.hpp"

#include <algorithm>
#include <cmath>

namespace alcedo {
namespace {

constexpr float kPi = 3.14159265358979323846f;

auto            Dot2(const QPointF& a, const QPointF& b) -> float {
  return (static_cast<float>(a.x()) * static_cast<float>(b.x())) +
         (static_cast<float>(a.y()) * static_cast<float>(b.y()));
}

auto VectorLengthSquared(const QPointF& vector) -> float { return Dot2(vector, vector); }

}  // namespace

auto CropGeometry::Clamp01(float value) -> float { return std::clamp(value, 0.0f, 1.0f); }

auto CropGeometry::NormalizeAngleDegrees(float angle_degrees) -> float {
  if (!std::isfinite(angle_degrees)) {
    return 0.0f;
  }
  angle_degrees = std::fmod(angle_degrees, 360.0f);
  if (angle_degrees > 180.0f) {
    angle_degrees -= 360.0f;
  } else if (angle_degrees < -180.0f) {
    angle_degrees += 360.0f;
  }
  return angle_degrees;
}

auto CropGeometry::ClampAspect(float aspect) -> float { return std::max(aspect, 1e-4f); }

auto CropGeometry::ClampAspectRatio(float aspect_ratio) -> float {
  return std::max(aspect_ratio, kCropMinSize);
}

auto CropGeometry::SafeAspect(int image_width, int image_height) -> float {
  if (image_width <= 0 || image_height <= 0) {
    return 1.0f;
  }
  return ClampAspect(static_cast<float>(image_width) / static_cast<float>(image_height));
}

auto CropGeometry::UvToMetric(const QPointF& uv, float aspect) -> QPointF {
  const float safe_aspect = ClampAspect(aspect);
  return QPointF(static_cast<float>(uv.x()) * safe_aspect, static_cast<float>(uv.y()));
}

auto CropGeometry::MetricToUv(const QPointF& metric, float aspect) -> QPointF {
  const float safe_aspect = ClampAspect(aspect);
  return QPointF(static_cast<float>(metric.x()) / safe_aspect, static_cast<float>(metric.y()));
}

auto CropGeometry::RotateVector(const QPointF& vector, float angle_degrees) -> QPointF {
  const float radians = NormalizeAngleDegrees(angle_degrees) * (kPi / 180.0f);
  const float cosine  = std::cos(radians);
  const float sine    = std::sin(radians);
  return QPointF(
      (cosine * static_cast<float>(vector.x())) - (sine * static_cast<float>(vector.y())),
      (sine * static_cast<float>(vector.x())) + (cosine * static_cast<float>(vector.y())));
}

auto CropGeometry::InverseRotateVector(const QPointF& vector, float angle_degrees) -> QPointF {
  const float radians = NormalizeAngleDegrees(angle_degrees) * (kPi / 180.0f);
  const float cosine  = std::cos(radians);
  const float sine    = std::sin(radians);
  return QPointF(
      (cosine * static_cast<float>(vector.x())) + (sine * static_cast<float>(vector.y())),
      (-sine * static_cast<float>(vector.x())) + (cosine * static_cast<float>(vector.y())));
}

auto CropGeometry::MakeRectFromCenterSize(const QPointF& center, float width, float height)
    -> QRectF {
  return QRectF(center.x() - (static_cast<qreal>(width) * 0.5),
                center.y() - (static_cast<qreal>(height) * 0.5), width, height);
}

auto CropGeometry::ClampCropRect(const QRectF& rect) -> QRectF {
  QRectF normalized_rect = rect.normalized();
  float  x               = Clamp01(static_cast<float>(normalized_rect.x()));
  float  y               = Clamp01(static_cast<float>(normalized_rect.y()));
  float  width  = std::clamp(static_cast<float>(normalized_rect.width()), kCropMinSize, 1.0f);
  float  height = std::clamp(static_cast<float>(normalized_rect.height()), kCropMinSize, 1.0f);
  x             = std::clamp(x, 0.0f, 1.0f - width);
  y             = std::clamp(y, 0.0f, 1.0f - height);
  return QRectF(x, y, width, height);
}

auto CropGeometry::MakeAspectLockedRectFromDiagonal(const QPointF& anchor_uv,
                                                    const QPointF& cursor_uv, float image_aspect,
                                                    float aspect_ratio) -> QRectF {
  const float   target_ratio  = ClampAspectRatio(aspect_ratio);
  const QPointF anchor_metric = UvToMetric(anchor_uv, image_aspect);
  const QPointF cursor_metric = UvToMetric(cursor_uv, image_aspect);
  const QPointF delta_metric  = cursor_metric - anchor_metric;
  const float   sign_x        = delta_metric.x() >= 0.0 ? 1.0f : -1.0f;
  const float   sign_y        = delta_metric.y() >= 0.0 ? 1.0f : -1.0f;
  const float   abs_width =
      std::max(kCropMinSize * image_aspect, std::abs(static_cast<float>(delta_metric.x())));
  const float   abs_height = std::max(kCropMinSize, std::abs(static_cast<float>(delta_metric.y())));
  const bool    width_limited = (abs_width / std::max(abs_height, kCropMinSize)) <= target_ratio;
  const float   rect_width_metric  = width_limited ? abs_width : (abs_height * target_ratio);
  const float   rect_height_metric = width_limited ? (abs_width / target_ratio) : abs_height;
  const QPointF corner_metric =
      anchor_metric + QPointF(sign_x * rect_width_metric, sign_y * rect_height_metric);
  return QRectF(MetricToUv(anchor_metric, image_aspect), MetricToUv(corner_metric, image_aspect))
      .normalized();
}

auto CropGeometry::ClampCropRectForRotation(const QRectF& rect, float angle_degrees, float aspect)
    -> QRectF {
  const float safe_aspect     = ClampAspect(aspect);
  QRectF      normalized_rect = rect.normalized();
  float       width  = std::clamp(static_cast<float>(normalized_rect.width()), kCropMinSize, 1.0f);
  float       height = std::clamp(static_cast<float>(normalized_rect.height()), kCropMinSize, 1.0f);
  QPointF     center_uv = normalized_rect.center();
  center_uv.setX(Clamp01(static_cast<float>(center_uv.x())));
  center_uv.setY(Clamp01(static_cast<float>(center_uv.y())));
  QPointF     center_metric = UvToMetric(center_uv, safe_aspect);

  float       half_width    = (width * safe_aspect) * 0.5f;
  float       half_height   = height * 0.5f;

  const float radians       = NormalizeAngleDegrees(angle_degrees) * (kPi / 180.0f);
  const float cosine        = std::abs(std::cos(radians));
  const float sine          = std::abs(std::sin(radians));

  float       extent_x      = (cosine * half_width) + (sine * half_height);
  float       extent_y      = (sine * half_width) + (cosine * half_height);
  if (extent_x > (safe_aspect * 0.5f) || extent_y > 0.5f) {
    const float scale_x = (extent_x > 0.0f) ? ((safe_aspect * 0.5f) / extent_x) : 1.0f;
    const float scale_y = (extent_y > 0.0f) ? (0.5f / extent_y) : 1.0f;
    const float scale   = std::clamp(std::min(scale_x, scale_y), 0.0f, 1.0f);
    half_width          = std::max((kCropMinSize * safe_aspect) * 0.5f, half_width * scale);
    half_height         = std::max(kCropMinSize * 0.5f, half_height * scale);
    extent_x            = (cosine * half_width) + (sine * half_height);
    extent_y            = (sine * half_width) + (cosine * half_height);
  }

  center_metric.setX(
      std::clamp(static_cast<float>(center_metric.x()), extent_x, safe_aspect - extent_x));
  center_metric.setY(std::clamp(static_cast<float>(center_metric.y()), extent_y, 1.0f - extent_y));

  const QPointF final_center_uv = MetricToUv(center_metric, safe_aspect);
  const float   final_width     = std::clamp((half_width * 2.0f) / safe_aspect, kCropMinSize, 1.0f);
  const float   final_height    = std::clamp(half_height * 2.0f, kCropMinSize, 1.0f);
  return MakeRectFromCenterSize(final_center_uv, final_width, final_height);
}

auto CropGeometry::RotatedCropCornersUv(const QRectF& rect, float angle_degrees, float aspect)
    -> std::array<QPointF, 4> {
  const float   safe_aspect   = ClampAspect(aspect);
  const QPointF center_metric = UvToMetric(rect.center(), safe_aspect);
  const float   half_width    = std::max((kCropMinSize * safe_aspect) * 0.5f,
                                         static_cast<float>(rect.width()) * safe_aspect * 0.5f);
  const float half_height = std::max(kCropMinSize * 0.5f, static_cast<float>(rect.height()) * 0.5f);
  const std::array<QPointF, 4> local = {
      QPointF(-half_width, -half_height), QPointF(half_width, -half_height),
      QPointF(half_width, half_height), QPointF(-half_width, half_height)};

  std::array<QPointF, 4> corners{};
  for (size_t i = 0; i < local.size(); ++i) {
    corners[i] = MetricToUv(center_metric + RotateVector(local[i], angle_degrees), safe_aspect);
  }
  return corners;
}

auto CropGeometry::IsPointInsideRotatedCrop(const QPointF& point_uv, const QRectF& rect,
                                            float angle_degrees, float aspect) -> bool {
  const float   safe_aspect = ClampAspect(aspect);
  const QPointF local       = InverseRotateVector(
      UvToMetric(point_uv, safe_aspect) - UvToMetric(rect.center(), safe_aspect), angle_degrees);
  const float half_width  = std::max((kCropMinSize * safe_aspect) * 0.5f,
                                     static_cast<float>(rect.width()) * safe_aspect * 0.5f);
  const float half_height = std::max(kCropMinSize * 0.5f, static_cast<float>(rect.height()) * 0.5f);
  return std::abs(static_cast<float>(local.x())) <= half_width &&
         std::abs(static_cast<float>(local.y())) <= half_height;
}

auto CropGeometry::PointSegmentDistanceSquared(const QPointF& point, const QPointF& a,
                                               const QPointF& b) -> float {
  const QPointF ab      = b - a;
  const float   ab_len2 = Dot2(ab, ab);
  if (ab_len2 <= 1e-8f) {
    const float dx = static_cast<float>(point.x() - a.x());
    const float dy = static_cast<float>(point.y() - a.y());
    return (dx * dx) + (dy * dy);
  }
  const float   t          = std::clamp(Dot2(point - a, ab) / ab_len2, 0.0f, 1.0f);
  const QPointF projection = a + (ab * t);
  const float   dx         = static_cast<float>(point.x() - projection.x());
  const float   dy         = static_cast<float>(point.y() - projection.y());
  return (dx * dx) + (dy * dy);
}

auto CropGeometry::LerpPoint(const QPointF& a, const QPointF& b, float t) -> QPointF {
  return QPointF(a.x() + (b.x() - a.x()) * t, a.y() + (b.y() - a.y()) * t);
}

auto CropGeometry::NormalizeVector(const QPointF& vector, const QPointF& fallback) -> QPointF {
  const float len2 = VectorLengthSquared(vector);
  if (len2 <= 1e-8f) {
    return fallback;
  }
  const float inv_len = 1.0f / std::sqrt(len2);
  return QPointF(static_cast<float>(vector.x()) * inv_len,
                 static_cast<float>(vector.y()) * inv_len);
}

auto CropGeometry::CropCenterWidgetPoint(const std::array<QPointF, 4>& corners) -> QPointF {
  return QPointF((corners[0].x() + corners[2].x()) * 0.5, (corners[0].y() + corners[2].y()) * 0.5);
}

auto CropGeometry::CropRotateHandleWidgetPoint(const std::array<QPointF, 4>& corners)
    -> std::pair<QPointF, QPointF> {
  const qreal   min_x = std::min({corners[0].x(), corners[1].x(), corners[2].x(), corners[3].x()});
  const qreal   max_x = std::max({corners[0].x(), corners[1].x(), corners[2].x(), corners[3].x()});
  const qreal   min_y = std::min({corners[0].y(), corners[1].y(), corners[2].y(), corners[3].y()});
  const qreal   max_y = std::max({corners[0].y(), corners[1].y(), corners[2].y(), corners[3].y()});
  const bool    portrait_crop = (max_y - min_y) > (max_x - min_x);

  const QPointF anchor        = portrait_crop ? LerpPoint(corners[1], corners[2], 0.5f)
                                              : LerpPoint(corners[0], corners[1], 0.5f);
  const QPointF center        = CropCenterWidgetPoint(corners);
  const QPointF dir =
      NormalizeVector(anchor - center, portrait_crop ? QPointF(1.0, 0.0) : QPointF(0.0, -1.0));
  const QPointF handle(anchor.x() + (dir.x() * kCropRotateHandleOffsetPx),
                       anchor.y() + (dir.y() * kCropRotateHandleOffsetPx));
  return {anchor, handle};
}

auto CropGeometry::CursorForCropHit(const CropHitTestResult& hit) -> OverlayCursor {
  if (hit.rotate_handle_hit) {
    return OverlayCursor::Rotate;
  }
  if (hit.corner_index >= 0 || hit.edge != CropEdge::None) {
    const OverlayCursor resize = OverlayResizeCursorForAxis(hit.resize_axis);
    if (resize != OverlayCursor::None) {
      return resize;
    }
    // No axis (a caller-built hit): fall back to the unrotated crop layout.
    if (hit.corner_index >= 0) {
      return (hit.corner_index == 0 || hit.corner_index == 2) ? OverlayCursor::ResizeDiagonalDown
                                                              : OverlayCursor::ResizeDiagonalUp;
    }
    return (hit.edge == CropEdge::Top || hit.edge == CropEdge::Bottom)
               ? OverlayCursor::ResizeVertical
               : OverlayCursor::ResizeHorizontal;
  }
  return hit.inside_crop ? OverlayCursor::Move : OverlayCursor::None;
}

auto CropGeometry::OppositeCropCornerIndex(int corner_index) -> int {
  switch (corner_index) {
    case 0:
      return 2;
    case 1:
      return 3;
    case 2:
      return 0;
    case 3:
      return 1;
    default:
      return -1;
  }
}

auto CropGeometry::ResizeRotatedCropFromFixedCorner(const QPointF& fixed_corner_uv,
                                                    const QPointF& cursor_uv, float angle_degrees,
                                                    float metric_aspect, bool aspect_locked,
                                                    float aspect_ratio) -> QRectF {
  const QPointF fixed_metric  = UvToMetric(fixed_corner_uv, metric_aspect);
  const QPointF cursor_metric = UvToMetric(cursor_uv, metric_aspect);
  const QPointF local_delta   = InverseRotateVector(cursor_metric - fixed_metric, angle_degrees);

  const float   sign_x        = local_delta.x() >= 0.0 ? 1.0f : -1.0f;
  const float   sign_y        = local_delta.y() >= 0.0 ? 1.0f : -1.0f;
  float         width_metric =
      std::max(kCropMinSize * metric_aspect, std::abs(static_cast<float>(local_delta.x())));
  float height_metric = std::max(kCropMinSize, std::abs(static_cast<float>(local_delta.y())));

  if (aspect_locked) {
    const float locked_ratio = ClampAspectRatio(aspect_ratio);
    const bool  width_limited =
        (width_metric / std::max(height_metric, kCropMinSize)) <= locked_ratio;
    if (width_limited) {
      height_metric = std::max(kCropMinSize, width_metric / locked_ratio);
    } else {
      width_metric = std::max(kCropMinSize * metric_aspect, height_metric * locked_ratio);
    }
  }

  const QPointF center_metric = fixed_metric + RotateVector(QPointF(sign_x * width_metric * 0.5f,
                                                                    sign_y * height_metric * 0.5f),
                                                            angle_degrees);
  const QPointF center_uv     = MetricToUv(center_metric, metric_aspect);
  const float   width_uv =
      std::max(kCropMinSize, width_metric / std::max(metric_aspect, kCropMinSize));
  const float height_uv = std::max(kCropMinSize, height_metric);
  return ClampCropRectForRotation(MakeRectFromCenterSize(center_uv, width_uv, height_uv),
                                  angle_degrees, metric_aspect);
}

auto CropGeometry::HitTestWidgetGeometry(const std::array<QPointF, 4>& corners_widget,
                                         const QPointF& event_pos) -> CropHitTestResult {
  CropHitTestResult hit{};

  float             best_corner_dist2 = kCropCornerHitRadiusPx * kCropCornerHitRadiusPx;
  for (int i = 0; i < static_cast<int>(corners_widget.size()); ++i) {
    const float dx = static_cast<float>(corners_widget[static_cast<size_t>(i)].x() - event_pos.x());
    const float dy = static_cast<float>(corners_widget[static_cast<size_t>(i)].y() - event_pos.y());
    const float d2 = (dx * dx) + (dy * dy);
    if (d2 <= best_corner_dist2) {
      best_corner_dist2 = d2;
      hit.corner_index  = i;
    }
  }

  const auto  handle_geom = CropRotateHandleWidgetPoint(corners_widget);
  const float handle_dx   = static_cast<float>(handle_geom.second.x() - event_pos.x());
  const float handle_dy   = static_cast<float>(handle_geom.second.y() - event_pos.y());
  const float handle_d2   = (handle_dx * handle_dx) + (handle_dy * handle_dy);
  hit.rotate_handle_hit =
      handle_d2 <= (kCropRotateHandleHitRadiusPx * kCropRotateHandleHitRadiusPx);

  const QPointF center = CropCenterWidgetPoint(corners_widget);
  if (hit.corner_index >= 0 && !hit.rotate_handle_hit) {
    hit.resize_axis = corners_widget[static_cast<size_t>(hit.corner_index)] - center;
  }
  if (hit.rotate_handle_hit || hit.corner_index >= 0) {
    return hit;
  }

  const float edge_hit_dist2 = kCropEdgeHitRadiusPx * kCropEdgeHitRadiusPx;
  const float top_d2 = PointSegmentDistanceSquared(event_pos, corners_widget[0], corners_widget[1]);
  const float right_d2 =
      PointSegmentDistanceSquared(event_pos, corners_widget[1], corners_widget[2]);
  const float bottom_d2 =
      PointSegmentDistanceSquared(event_pos, corners_widget[2], corners_widget[3]);
  const float left_d2 =
      PointSegmentDistanceSquared(event_pos, corners_widget[3], corners_widget[0]);

  float      min_edge_d2 = edge_hit_dist2;
  const auto try_edge    = [&](float d2, CropEdge edge) {
    if (d2 <= min_edge_d2) {
      min_edge_d2 = d2;
      hit.edge    = edge;
    }
  };
  try_edge(top_d2, CropEdge::Top);
  try_edge(right_d2, CropEdge::Right);
  try_edge(bottom_d2, CropEdge::Bottom);
  try_edge(left_d2, CropEdge::Left);
  const auto edge_midpoint = [&](int a, int b) {
    return LerpPoint(corners_widget[static_cast<size_t>(a)],
                     corners_widget[static_cast<size_t>(b)], 0.5f);
  };
  switch (hit.edge) {
    case CropEdge::Top:
      hit.resize_axis = edge_midpoint(0, 1) - center;
      break;
    case CropEdge::Right:
      hit.resize_axis = edge_midpoint(1, 2) - center;
      break;
    case CropEdge::Bottom:
      hit.resize_axis = edge_midpoint(2, 3) - center;
      break;
    case CropEdge::Left:
      hit.resize_axis = edge_midpoint(3, 0) - center;
      break;
    case CropEdge::None:
      break;
  }
  return hit;
}

}  // namespace alcedo
