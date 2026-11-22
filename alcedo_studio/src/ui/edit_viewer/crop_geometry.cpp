//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/edit_viewer/crop_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "edit/geometry/crop_frame.hpp"

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
  return NormalizeRotationDegrees(angle_degrees);
}

auto CropGeometry::ClampAspectRatio(float aspect_ratio) -> float {
  return std::max(aspect_ratio, kCropMinSize);
}

auto CropGeometry::ToNormalizedRect(const QRectF& rect) -> NormalizedRect {
  return NormalizedRect{static_cast<float>(rect.x()), static_cast<float>(rect.y()),
                        static_cast<float>(rect.width()), static_cast<float>(rect.height())};
}

auto CropGeometry::ToQRectF(const NormalizedRect& rect) -> QRectF {
  return QRectF(rect.x, rect.y, rect.w, rect.h);
}

auto CropGeometry::ClampCrop(const QRectF& crop, float angle_degrees, Extent2D source) -> QRectF {
  return ToQRectF(ClampCropToRotatedSource(ToNormalizedRect(crop.normalized()), angle_degrees,
                                           source));
}

auto CropGeometry::CropCornersInReference(const QRectF& crop, float angle_degrees,
                                          Extent2D source) -> std::array<Vector2, 4> {
  return CropFrameCornersInReference(ToNormalizedRect(crop), angle_degrees, source);
}

auto CropGeometry::ReferenceToFrame(Vector2 reference, float angle_degrees) -> QPointF {
  const auto frame = TransformPoint(
      Matrix3x3::Rotate(NormalizeAngleDegrees(angle_degrees) * (kPi / 180.0f)), reference);
  return QPointF(frame.x, frame.y);
}

auto CropGeometry::FrameToReference(const QPointF& frame, float angle_degrees) -> Vector2 {
  return TransformPoint(
      Matrix3x3::Rotate(-NormalizeAngleDegrees(angle_degrees) * (kPi / 180.0f)),
      Vector2{static_cast<float>(frame.x()), static_cast<float>(frame.y())});
}

auto CropGeometry::FrameBoxFromCrop(const QRectF& crop, float angle_degrees, Extent2D source)
    -> QRectF {
  const float   full_w = static_cast<float>(source.width);
  const float   full_h = static_cast<float>(source.height);
  const Vector2 center_reference{static_cast<float>(crop.center().x()) * full_w,
                                 static_cast<float>(crop.center().y()) * full_h};
  const QPointF center = ReferenceToFrame(center_reference, angle_degrees);
  const qreal   width  = crop.width() * full_w;
  const qreal   height = crop.height() * full_h;
  return QRectF(center.x() - width * 0.5, center.y() - height * 0.5, width, height);
}

auto CropGeometry::CropFromFrameBox(const QRectF& box, float angle_degrees, Extent2D source)
    -> QRectF {
  const QRectF  normalized_box = box.normalized();
  const float   full_w         = static_cast<float>(source.width);
  const float   full_h         = static_cast<float>(source.height);
  const Vector2 center         = FrameToReference(normalized_box.center(), angle_degrees);
  const qreal   width          = normalized_box.width() / full_w;
  const qreal   height         = normalized_box.height() / full_h;
  const QRectF  crop(center.x / full_w - width * 0.5, center.y / full_h - height * 0.5, width,
                     height);
  return ClampCrop(crop, angle_degrees, source);
}

auto CropGeometry::MakeAspectLockedBoxFromDiagonal(const QPointF& anchor, const QPointF& cursor,
                                                   float aspect_ratio) -> QRectF {
  const float   ratio  = ClampAspectRatio(aspect_ratio);
  const QPointF delta  = cursor - anchor;
  const float   sign_x = delta.x() >= 0.0 ? 1.0f : -1.0f;
  const float   sign_y = delta.y() >= 0.0 ? 1.0f : -1.0f;
  float width  = std::max(kCropMinFramePixels, std::abs(static_cast<float>(delta.x())));
  float height = std::max(kCropMinFramePixels, std::abs(static_cast<float>(delta.y())));
  if (width / height <= ratio) {
    height = width / ratio;
  } else {
    width = height * ratio;
  }
  return QRectF(anchor, anchor + QPointF(sign_x * width, sign_y * height)).normalized();
}

auto CropGeometry::ResizeBoxFromFixedCorner(const QPointF& fixed_corner, const QPointF& cursor,
                                            bool aspect_locked, float aspect_ratio) -> QRectF {
  if (aspect_locked) {
    return MakeAspectLockedBoxFromDiagonal(fixed_corner, cursor, aspect_ratio);
  }
  const QPointF delta  = cursor - fixed_corner;
  const qreal   sign_x = delta.x() >= 0.0 ? 1.0 : -1.0;
  const qreal   sign_y = delta.y() >= 0.0 ? 1.0 : -1.0;
  const qreal   width  = std::max<qreal>(kCropMinFramePixels, std::abs(delta.x()));
  const qreal   height = std::max<qreal>(kCropMinFramePixels, std::abs(delta.y()));
  return QRectF(fixed_corner, fixed_corner + QPointF(sign_x * width, sign_y * height))
      .normalized();
}

auto CropGeometry::ResizeBoxEdge(const QRectF& box, CropEdge edge, const QPointF& cursor,
                                 bool aspect_locked, float aspect_ratio) -> QRectF {
  qreal left   = box.left();
  qreal right  = box.right();
  qreal top    = box.top();
  qreal bottom = box.bottom();
  switch (edge) {
    case CropEdge::Left:
      left = std::min(cursor.x(), right - kCropMinFramePixels);
      break;
    case CropEdge::Right:
      right = std::max(cursor.x(), left + kCropMinFramePixels);
      break;
    case CropEdge::Top:
      top = std::min(cursor.y(), bottom - kCropMinFramePixels);
      break;
    case CropEdge::Bottom:
      bottom = std::max(cursor.y(), top + kCropMinFramePixels);
      break;
    case CropEdge::None:
      return box;
  }
  if (aspect_locked) {
    const qreal ratio = ClampAspectRatio(aspect_ratio);
    if (edge == CropEdge::Left || edge == CropEdge::Right) {
      const qreal half_height = (right - left) / ratio * 0.5;
      const qreal center_y    = box.center().y();
      top                     = center_y - half_height;
      bottom                  = center_y + half_height;
    } else {
      const qreal half_width = (bottom - top) * ratio * 0.5;
      const qreal center_x   = box.center().x();
      left                   = center_x - half_width;
      right                  = center_x + half_width;
    }
  }
  return QRectF(QPointF(left, top), QPointF(right, bottom));
}

auto CropGeometry::IsPointInsideQuad(const std::array<QPointF, 4>& corners, const QPointF& point)
    -> bool {
  bool has_negative = false;
  bool has_positive = false;
  for (size_t i = 0; i < corners.size(); ++i) {
    const QPointF& a     = corners[i];
    const QPointF& b     = corners[(i + 1) % corners.size()];
    const qreal    cross = (b.x() - a.x()) * (point.y() - a.y()) - (b.y() - a.y()) * (point.x() - a.x());
    has_negative         = has_negative || cross < 0.0;
    has_positive         = has_positive || cross > 0.0;
  }
  return !(has_negative && has_positive);
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

auto CropGeometry::CropAngleBadgeWidgetPoint(const std::array<QPointF, 4>& corners) -> QPointF {
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
  return QPointF(anchor.x() + (dir.x() * kCropAngleBadgeOffsetPx),
                 anchor.y() + (dir.y() * kCropAngleBadgeOffsetPx));
}

auto CropGeometry::CursorForCropHit(const CropHitTestResult& hit) -> OverlayCursor {
  if (hit.rotate_hit) {
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

auto CropGeometry::HitTestWidgetGeometry(const std::array<QPointF, 4>& corners_widget,
                                         const QPointF& event_pos) -> CropHitTestResult {
  CropHitTestResult hit{};
  const QPointF     center            = CropCenterWidgetPoint(corners_widget);

  float             best_corner_dist2 = kCropCornerHitRadiusPx * kCropCornerHitRadiusPx;
  float             nearest_corner_d2 = std::numeric_limits<float>::max();
  for (int i = 0; i < static_cast<int>(corners_widget.size()); ++i) {
    const float d2    = VectorLengthSquared(corners_widget[static_cast<size_t>(i)] - event_pos);
    nearest_corner_d2 = std::min(nearest_corner_d2, d2);
    if (d2 <= best_corner_dist2) {
      best_corner_dist2 = d2;
      hit.corner_index  = i;
    }
  }
  if (hit.corner_index >= 0) {
    hit.resize_axis = corners_widget[static_cast<size_t>(hit.corner_index)] - center;
    return hit;
  }

  // Edge bands are asymmetric: wider outside the frame, so a press that lands a little beyond
  // the edge still takes the edge. Edges are top, right, bottom, left (corners i to i + 1).
  constexpr std::array<CropEdge, 4> kEdges = {CropEdge::Top, CropEdge::Right, CropEdge::Bottom,
                                              CropEdge::Left};
  float                             best_edge_abs = std::numeric_limits<float>::max();
  for (size_t i = 0; i < kEdges.size(); ++i) {
    const QPointF& a       = corners_widget[i];
    const QPointF& b       = corners_widget[(i + 1) % corners_widget.size()];
    const QPointF  ab      = b - a;
    const float    ab_len2 = VectorLengthSquared(ab);
    if (ab_len2 <= 1e-8f) {
      continue;
    }
    const float along = Dot2(event_pos - a, ab) / ab_len2;
    if (along < 0.0f || along > 1.0f) {
      continue;
    }
    QPointF       normal   = NormalizeVector(QPointF(ab.y(), -ab.x()), QPointF(0.0, -1.0));
    const QPointF midpoint = LerpPoint(a, b, 0.5f);
    if (Dot2(normal, midpoint - center) < 0.0f) {
      normal = -normal;
    }
    const float outward = Dot2(event_pos - a, normal);
    if (outward < -kCropEdgeHitInsidePx || outward > kCropEdgeHitOutsidePx) {
      continue;
    }
    if (std::abs(outward) < best_edge_abs) {
      best_edge_abs   = std::abs(outward);
      hit.edge        = kEdges[i];
      hit.resize_axis = midpoint - center;
    }
  }
  if (hit.edge != CropEdge::None) {
    return hit;
  }

  hit.rotate_hit = !IsPointInsideQuad(corners_widget, event_pos) &&
                   nearest_corner_d2 <= kCropRotateZoneRadiusPx * kCropRotateZoneRadiusPx;
  return hit;
}

}  // namespace alcedo
