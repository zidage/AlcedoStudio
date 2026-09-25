//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QPointF>
#include <QRectF>
#include <Qt>
#include <array>
#include <utility>

#include "ui/edit_viewer/overlay_cursor.hpp"

namespace alcedo {

enum class CropCorner {
  None,
  TopLeft,
  TopRight,
  BottomRight,
  BottomLeft,
};

enum class CropEdge {
  None,
  Top,
  Right,
  Bottom,
  Left,
};

struct CropHitTestResult {
  int      corner_index      = -1;
  CropEdge edge              = CropEdge::None;
  bool     rotate_handle_hit = false;
  bool     inside_crop       = false;
  /// Widget-space drag axis of the hit corner or edge: crop center to the
  /// corner, or to the edge midpoint. Zero when no corner or edge is hit.
  QPointF  resize_axis{};
};

class CropGeometry {
 public:
  static constexpr float kCropMinSize                  = 1e-4f;
  static constexpr float kCropCornerHitRadiusPx        = 12.0f;
  static constexpr float kCropEdgeHitRadiusPx          = 10.0f;
  static constexpr float kCropCornerDrawRadiusPx       = 4.0f;
  static constexpr float kCropRotateHandleOffsetPx     = 28.0f;
  static constexpr float kCropRotateHandleHitRadiusPx  = 14.0f;
  static constexpr float kCropRotateHandleDrawRadiusPx = 5.0f;

  static auto            Clamp01(float value) -> float;
  static auto            NormalizeAngleDegrees(float angle_degrees) -> float;
  static auto            ClampAspect(float aspect) -> float;
  static auto            ClampAspectRatio(float aspect_ratio) -> float;
  static auto            SafeAspect(int image_width, int image_height) -> float;

  static auto            UvToMetric(const QPointF& uv, float aspect) -> QPointF;
  static auto            MetricToUv(const QPointF& metric, float aspect) -> QPointF;
  static auto            RotateVector(const QPointF& vector, float angle_degrees) -> QPointF;
  static auto            InverseRotateVector(const QPointF& vector, float angle_degrees) -> QPointF;
  static auto MakeRectFromCenterSize(const QPointF& center, float width, float height) -> QRectF;
  static auto ClampCropRect(const QRectF& rect) -> QRectF;
  static auto MakeAspectLockedRectFromDiagonal(const QPointF& anchor_uv, const QPointF& cursor_uv,
                                               float image_aspect, float aspect_ratio) -> QRectF;
  static auto ClampCropRectForRotation(const QRectF& rect, float angle_degrees, float aspect)
      -> QRectF;
  static auto RotatedCropCornersUv(const QRectF& rect, float angle_degrees, float aspect)
      -> std::array<QPointF, 4>;
  static auto IsPointInsideRotatedCrop(const QPointF& point_uv, const QRectF& rect,
                                       float angle_degrees, float aspect) -> bool;
  static auto PointSegmentDistanceSquared(const QPointF& point, const QPointF& a, const QPointF& b)
      -> float;
  static auto LerpPoint(const QPointF& a, const QPointF& b, float t) -> QPointF;
  static auto NormalizeVector(const QPointF& vector, const QPointF& fallback) -> QPointF;
  static auto CropCenterWidgetPoint(const std::array<QPointF, 4>& corners) -> QPointF;
  static auto CropRotateHandleWidgetPoint(const std::array<QPointF, 4>& corners)
      -> std::pair<QPointF, QPointF>;
  /// Cursor for @p hit: rotate, a resize along its drag axis, or move inside.
  static auto CursorForCropHit(const CropHitTestResult& hit) -> OverlayCursor;
  static auto OppositeCropCornerIndex(int corner_index) -> int;
  static auto ResizeRotatedCropFromFixedCorner(const QPointF& fixed_corner_uv,
                                               const QPointF& cursor_uv, float angle_degrees,
                                               float metric_aspect, bool aspect_locked,
                                               float aspect_ratio) -> QRectF;
  static auto HitTestWidgetGeometry(const std::array<QPointF, 4>& corners_widget,
                                    const QPointF&                event_pos) -> CropHitTestResult;
};

}  // namespace alcedo
