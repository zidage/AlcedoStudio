//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/edit_viewer/ai_mask_input_overlay_geometry.hpp"

#include <QPainterPath>
#include <QPolygonF>
#include <algorithm>

#include "ui/edit_viewer/overlay_triangle_primitives.hpp"

namespace alcedo {

using namespace overlay_triangles;

namespace {

/// Crosshair arms of the Point and Box cursors: from the gap radius to the arm radius.
constexpr float kCursorGapRadius     = 4.0f;
constexpr float kCursorArmRadius     = 11.0f;
constexpr float kCursorLineWidth     = 1.5f;
/// Ring of the Path cursor, in multiples of the Path stroke width.
constexpr float kPathCursorRingScale = 1.5f;
/// Offset of the Add/Subtract badge from the cursor hot spot, and its radius.
constexpr float kBadgeOffset         = 12.0f;
constexpr float kBadgeRadius         = 5.5f;
constexpr float kGuideDashLength     = 6.0f;
constexpr float kGuideGapLength      = 4.0f;

/// A stroke with round joins and caps: a dark rim, then the core.
void            AppendRimmedPolyline(std::vector<MaskOverlayVertex>& triangles,
                                     const std::vector<QPointF>& points, bool closed, float core_width,
                                     const QColor& core, const AiMaskInputOverlayStyle& style,
                                     const QRectF& clip) {
  if (points.empty()) {
    return;
  }
  const float aa = style.antialias_width;
  for (const auto& [width, color] :
       {std::pair{core_width + 2.0f * style.outline_width, style.outline_color},
                   std::pair{core_width, core}}) {
    if (points.size() == 1) {
      AppendFilledDisc(triangles, points.front(), width * 0.5f, aa, color, clip);
      continue;
    }
    const std::size_t count = closed ? points.size() : points.size() - 1;
    for (std::size_t index = 0; index < count; ++index) {
      AppendClippedStroke(triangles, points[index], points[(index + 1) % points.size()], width, aa,
                                     color, clip, /*round_caps=*/true);
    }
  }
}

void AppendRimmedDashedPolyline(std::vector<MaskOverlayVertex>& triangles,
                                const std::vector<QPointF>& points, bool closed, float core_width,
                                const QColor& core, const AiMaskInputOverlayStyle& style,
                                const QRectF& clip) {
  const float aa = style.antialias_width;
  AppendDashedPolylineStroke(triangles, points, closed, core_width + 2.0f * style.outline_width, aa,
                             style.outline_color, clip, kGuideDashLength, kGuideGapLength);
  AppendDashedPolylineStroke(triangles, points, closed, core_width, aa, core, clip,
                             kGuideDashLength, kGuideGapLength);
}

/// A disc with a dark rim, its sign color, and a light plus or minus.
void AppendSignedDisc(std::vector<MaskOverlayVertex>& triangles, const QPointF& center,
                      float radius, bool add, const AiMaskInputOverlayStyle& style,
                      const QRectF& clip) {
  const float aa = style.antialias_width;
  AppendFilledDisc(triangles, center, radius + style.outline_width, aa, style.outline_color, clip);
  AppendFilledDisc(triangles, center, radius, aa, add ? style.add_color : style.subtract_color,
                   clip);
  const float half  = radius * 0.55f;
  const float width = std::max(1.5f, radius * 0.3f);
  AppendClippedStroke(triangles, center - QPointF(half, 0.0), center + QPointF(half, 0.0), width,
                      aa, style.line_color, clip, /*round_caps=*/false);
  if (add) {
    AppendClippedStroke(triangles, center - QPointF(0.0, half), center + QPointF(0.0, half), width,
                        aa, style.line_color, clip, /*round_caps=*/false);
  }
}

void AppendCrosshair(std::vector<MaskOverlayVertex>& triangles, const QPointF& center,
                     const QColor& color, const AiMaskInputOverlayStyle& style) {
  for (const auto& [dx, dy] :
       {std::pair{1.0, 0.0}, std::pair{-1.0, 0.0}, std::pair{0.0, 1.0}, std::pair{0.0, -1.0}}) {
    const QPointF direction(dx, dy);
    AppendRimmedPolyline(
        triangles, {center + direction * kCursorGapRadius, center + direction * kCursorArmRadius},
        /*closed=*/false, kCursorLineWidth, color, style, {});
  }
}

void AppendCursor(std::vector<MaskOverlayVertex>&  triangles,
                  const AiMaskInputOverlayDisplay& display, const AiMaskInputOverlayStyle& style) {
  const QPointF& center = display.cursor_item;
  if (!IsFinitePoint(center)) {
    return;
  }
  const QColor& color = display.cursor_busy ? style.inactive_color : style.line_color;
  switch (display.cursor) {
    case AiMaskInputCursorKind::None:
      return;
    case AiMaskInputCursorKind::Point:
      AppendCrosshair(triangles, center, color, style);
      AppendRimmedPolyline(triangles, {center}, /*closed=*/false, kCursorLineWidth * 1.5f, color,
                           style, {});
      break;
    case AiMaskInputCursorKind::Box: {
      // Guides across the photograph help to align the box with the object.
      const QRectF& photo = display.photo_rect;
      if (photo.isValid() && !photo.isEmpty()) {
        AppendRimmedDashedPolyline(
            triangles, {QPointF(photo.left(), center.y()), QPointF(photo.right(), center.y())},
            /*closed=*/false, 1.0f, color, style, photo);
        AppendRimmedDashedPolyline(
            triangles, {QPointF(center.x(), photo.top()), QPointF(center.x(), photo.bottom())},
            /*closed=*/false, 1.0f, color, style, photo);
      }
      AppendCrosshair(triangles, center, color, style);
      break;
    }
    case AiMaskInputCursorKind::Path: {
      const float aa     = style.antialias_width;
      const float radius = style.stroke_width * kPathCursorRingScale;
      AppendHollowCircle(triangles, center, radius, kCursorLineWidth + 2.0f * style.outline_width,
                         aa, style.outline_color, {});
      AppendHollowCircle(triangles, center, radius, kCursorLineWidth, aa, color, {});
      AppendRimmedPolyline(triangles, {center}, /*closed=*/false, kCursorLineWidth, color, style,
                           {});
      break;
    }
  }
  if (!display.cursor_busy) {
    AppendSignedDisc(triangles, center + QPointF(kBadgeOffset, kBadgeOffset), kBadgeRadius,
                     display.cursor_add, style, {});
  }
}

}  // namespace

auto AiMaskPersonUnionOutlines(const std::vector<QRectF>& normalized_boxes,
                               const QTransform&          analysis_to_item)
    -> std::vector<std::vector<QPointF>> {
  QPainterPath united;
  for (const auto& box : normalized_boxes) {
    const auto normalized = box.normalized();
    if (normalized.isEmpty()) {
      continue;
    }
    QPainterPath rect;
    rect.addRect(normalized);
    united = united.united(rect);
  }
  std::vector<std::vector<QPointF>> outlines;
  for (const auto& polygon : united.simplified().toSubpathPolygons(analysis_to_item)) {
    std::vector<QPointF> outline(polygon.begin(), polygon.end());
    // A subpath polygon repeats its first point at the end.
    if (outline.size() > 1 && outline.front() == outline.back()) {
      outline.pop_back();
    }
    if (outline.size() >= 3) {
      outlines.push_back(std::move(outline));
    }
  }
  return outlines;
}

auto BuildAiMaskInputOverlayTriangles(const AiMaskInputOverlayDisplay& display,
                                      const AiMaskInputOverlayStyle&   style)
    -> std::vector<MaskOverlayVertex> {
  std::vector<MaskOverlayVertex> triangles;
  const QRectF&                  clip = display.photo_rect;
  for (const auto& outline : display.person_outlines) {
    AppendRimmedPolyline(triangles, outline, /*closed=*/true, style.frame_width, style.line_color,
                         style, clip);
  }
  if (!display.highlighted_person.empty()) {
    AppendRimmedDashedPolyline(triangles, display.highlighted_person, /*closed=*/true,
                               style.frame_width, style.line_color, style, clip);
  }
  for (const auto& box : display.boxes) {
    AppendRimmedPolyline(triangles, {box.corners.begin(), box.corners.end()}, /*closed=*/true,
                         style.frame_width, box.add ? style.add_color : style.subtract_color, style,
                         clip);
  }
  for (const auto& stroke : display.strokes) {
    AppendRimmedPolyline(triangles, stroke.item, /*closed=*/false, style.stroke_width,
                         stroke.add ? style.add_color : style.subtract_color, style, clip);
  }
  for (const auto& point : display.points) {
    AppendSignedDisc(triangles, point.item, style.marker_radius, point.add, style, clip);
  }
  AppendCursor(triangles, display, style);
  return triangles;
}

}  // namespace alcedo
