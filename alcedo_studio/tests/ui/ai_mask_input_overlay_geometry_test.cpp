//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// SM9: the viewport presentation of the AI Mask tool input (markers, strokes, boxes, the outline
// of the selected people, and the tool cursor).

#include "ui/edit_viewer/ai_mask_input_overlay_geometry.hpp"

#include <gtest/gtest.h>

#include <QColor>
#include <QPointF>
#include <QRectF>
#include <QTransform>
#include <algorithm>
#include <cmath>
#include <vector>

namespace alcedo {
namespace {

/// Whether @p triangles has an opaque vertex of @p color (premultiplied at alpha 255).
[[nodiscard]] auto HasOpaqueColor(const std::vector<MaskOverlayVertex>& triangles,
                                  const QColor&                         color) -> bool {
  return std::any_of(triangles.begin(), triangles.end(), [&](const MaskOverlayVertex& vertex) {
    return vertex.a == 255 && vertex.r == color.red() && vertex.g == color.green() &&
           vertex.b == color.blue();
  });
}

[[nodiscard]] auto HasPoint(const std::vector<QPointF>& outline, QPointF point) -> bool {
  return std::any_of(outline.begin(), outline.end(), [&](const QPointF& vertex) {
    return std::abs(vertex.x() - point.x()) < 1.0e-6 && std::abs(vertex.y() - point.y()) < 1.0e-6;
  });
}

}  // namespace

TEST(MaskEditGeometryTest, PersonBoxesUniteIntoOneOutlinePerConnectedGroup) {
  const QTransform          to_item = QTransform::fromScale(1000.0, 1000.0);
  // A and B overlap; the small box lies inside A; C is apart.
  const std::vector<QRectF> boxes   = {
      QRectF(QPointF(0.1, 0.1), QPointF(0.4, 0.5)), QRectF(QPointF(0.3, 0.3), QPointF(0.6, 0.7)),
      QRectF(QPointF(0.15, 0.15), QPointF(0.2, 0.2)), QRectF(QPointF(0.8, 0.1), QPointF(0.9, 0.2))};

  const auto outlines = AiMaskPersonUnionOutlines(boxes, to_item);

  ASSERT_EQ(outlines.size(), 2U);
  const auto& united = outlines[0].size() > outlines[1].size() ? outlines[0] : outlines[1];
  const auto& apart  = outlines[0].size() > outlines[1].size() ? outlines[1] : outlines[0];
  ASSERT_EQ(united.size(), 8U);
  for (const auto corner :
       {QPointF(100, 100), QPointF(400, 100), QPointF(400, 300), QPointF(600, 300),
        QPointF(600, 700), QPointF(300, 700), QPointF(300, 500), QPointF(100, 500)}) {
    EXPECT_TRUE(HasPoint(united, corner)) << corner.x() << ", " << corner.y();
  }
  // The inner corners of the overlap are not on the outline.
  EXPECT_FALSE(HasPoint(united, QPointF(300, 300)));
  EXPECT_FALSE(HasPoint(united, QPointF(400, 500)));
  ASSERT_EQ(apart.size(), 4U);
  EXPECT_TRUE(HasPoint(apart, QPointF(800, 100)));
  EXPECT_TRUE(HasPoint(apart, QPointF(900, 200)));

  EXPECT_TRUE(AiMaskPersonUnionOutlines({}, to_item).empty());
}

TEST(MaskEditGeometryTest, PromptMarkersUseTheColorOfTheirMode) {
  const AiMaskInputOverlayStyle style;
  AiMaskInputOverlayDisplay     add;
  add.photo_rect = QRectF(0, 0, 200, 100);
  add.points.push_back(AiMaskInputPointMarker{QPointF(50, 50), true});
  const auto add_triangles = BuildAiMaskInputOverlayTriangles(add, style);
  EXPECT_TRUE(HasOpaqueColor(add_triangles, style.add_color));
  EXPECT_FALSE(HasOpaqueColor(add_triangles, style.subtract_color));
  // The plus glyph and the dark rim are drawn too.
  EXPECT_TRUE(HasOpaqueColor(add_triangles, style.line_color));
  EXPECT_TRUE(HasOpaqueColor(add_triangles, style.outline_color));

  AiMaskInputOverlayDisplay subtract = add;
  subtract.points.front().add        = false;
  const auto subtract_triangles      = BuildAiMaskInputOverlayTriangles(subtract, style);
  EXPECT_TRUE(HasOpaqueColor(subtract_triangles, style.subtract_color));
  EXPECT_FALSE(HasOpaqueColor(subtract_triangles, style.add_color));
  // The minus has one bar, the plus two.
  EXPECT_LT(subtract_triangles.size(), add_triangles.size());
}

TEST(MaskEditGeometryTest, InputOutsideThePhotoIsClippedButTheCursorIsNot) {
  const AiMaskInputOverlayStyle style;
  AiMaskInputOverlayDisplay     display;
  display.photo_rect = QRectF(0, 0, 100, 100);
  display.points.push_back(AiMaskInputPointMarker{QPointF(-200, -200), true});
  display.strokes.push_back(AiMaskInputStroke{{QPointF(300, 300), QPointF(400, 300)}, true});
  EXPECT_TRUE(BuildAiMaskInputOverlayTriangles(display, style).empty());

  display.cursor      = AiMaskInputCursorKind::Path;
  display.cursor_item = QPointF(-200, -200);
  EXPECT_FALSE(BuildAiMaskInputOverlayTriangles(display, style).empty());

  EXPECT_TRUE(BuildAiMaskInputOverlayTriangles(AiMaskInputOverlayDisplay{}, style).empty());
}

TEST(MaskEditGeometryTest, BusyCursorUsesTheInactiveColorWithoutModeBadge) {
  const AiMaskInputOverlayStyle style;
  for (const auto kind :
       {AiMaskInputCursorKind::Point, AiMaskInputCursorKind::Box, AiMaskInputCursorKind::Path}) {
    AiMaskInputOverlayDisplay display;
    display.photo_rect  = QRectF(0, 0, 400, 300);
    display.cursor      = kind;
    display.cursor_item = QPointF(200, 150);
    display.cursor_add  = false;
    const auto ready    = BuildAiMaskInputOverlayTriangles(display, style);
    EXPECT_TRUE(HasOpaqueColor(ready, style.subtract_color)) << static_cast<int>(kind);
    EXPECT_TRUE(HasOpaqueColor(ready, style.line_color)) << static_cast<int>(kind);

    display.cursor_busy = true;
    const auto busy     = BuildAiMaskInputOverlayTriangles(display, style);
    EXPECT_TRUE(HasOpaqueColor(busy, style.inactive_color)) << static_cast<int>(kind);
    EXPECT_FALSE(HasOpaqueColor(busy, style.subtract_color)) << static_cast<int>(kind);
    EXPECT_FALSE(HasOpaqueColor(busy, style.line_color)) << static_cast<int>(kind);
  }
}

}  // namespace alcedo
