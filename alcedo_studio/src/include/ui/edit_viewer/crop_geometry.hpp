//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QPointF>
#include <QRectF>
#include <Qt>
#include <array>
#include <utility>

#include "edit/geometry/types.hpp"
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

/**
 * @brief Crop frame math for the Geometry overlay.
 *
 * The document crop (`QRectF` in source-normalized units, see ImageGeometryModel) is an
 * axis-aligned rectangle of the output. Edits happen in *frame space*: source reference pixels
 * rotated by the document rotation (`Rotate(theta)`, the rotation ResolveRenderGeometry applies).
 * In frame space the crop frame is an axis-aligned box of `w * W` by `h * H` pixels, so create,
 * move, resize, and aspect lock are plain rectangle operations. Every result is converted back and
 * constrained with ClampCropToRotatedSource, the constraint the render applies.
 */
class CropGeometry {
 public:
  static constexpr float kCropMinSize                  = 1e-4f;
  static constexpr float kCropCornerHitRadiusPx        = 12.0f;
  static constexpr float kCropEdgeHitRadiusPx          = 10.0f;
  static constexpr float kCropCornerDrawRadiusPx       = 4.0f;
  static constexpr float kCropRotateHandleOffsetPx     = 28.0f;
  static constexpr float kCropRotateHandleHitRadiusPx  = 14.0f;
  static constexpr float kCropRotateHandleDrawRadiusPx = 5.0f;
  /// Smallest crop box side in frame-space (source) pixels while dragging.
  static constexpr float kCropMinFramePixels           = 1.0f;

  static auto            Clamp01(float value) -> float;
  static auto            NormalizeAngleDegrees(float angle_degrees) -> float;
  static auto            ClampAspectRatio(float aspect_ratio) -> float;

  static auto            ToNormalizedRect(const QRectF& rect) -> NormalizedRect;
  static auto            ToQRectF(const NormalizedRect& rect) -> QRectF;

  /// Constrain @p crop so its rotated corners stay inside @p source (ClampCropToRotatedSource).
  static auto ClampCrop(const QRectF& crop, float angle_degrees, Extent2D source) -> QRectF;
  /// Crop frame corners in reference pixels, clockwise from the output's top-left.
  static auto CropCornersInReference(const QRectF& crop, float angle_degrees, Extent2D source)
      -> std::array<Vector2, 4>;
  static auto ReferenceToFrame(Vector2 reference, float angle_degrees) -> QPointF;
  static auto FrameToReference(const QPointF& frame, float angle_degrees) -> Vector2;
  /// The crop frame as an axis-aligned frame-space box.
  static auto FrameBoxFromCrop(const QRectF& crop, float angle_degrees, Extent2D source) -> QRectF;
  /// Inverse of @ref FrameBoxFromCrop, followed by @ref ClampCrop.
  static auto CropFromFrameBox(const QRectF& box, float angle_degrees, Extent2D source) -> QRectF;

  /// Box spanned by @p anchor and @p cursor with width / height equal to @p aspect_ratio.
  static auto MakeAspectLockedBoxFromDiagonal(const QPointF& anchor, const QPointF& cursor,
                                              float aspect_ratio) -> QRectF;
  /// Box between a fixed corner and the cursor, optionally aspect-locked.
  static auto ResizeBoxFromFixedCorner(const QPointF& fixed_corner, const QPointF& cursor,
                                       bool aspect_locked, float aspect_ratio) -> QRectF;
  /// @p box with @p edge moved to @p cursor. With a lock, the other side follows about the
  /// box's center line.
  static auto ResizeBoxEdge(const QRectF& box, CropEdge edge, const QPointF& cursor,
                            bool aspect_locked, float aspect_ratio) -> QRectF;

  static auto IsPointInsideQuad(const std::array<QPointF, 4>& corners, const QPointF& point)
      -> bool;
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
  static auto HitTestWidgetGeometry(const std::array<QPointF, 4>& corners_widget,
                                    const QPointF&                event_pos) -> CropHitTestResult;
};

}  // namespace alcedo
