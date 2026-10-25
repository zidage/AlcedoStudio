//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QColor>
#include <QPointF>
#include <QRectF>
#include <QTransform>
#include <array>
#include <cstdint>
#include <vector>

#include "ui/edit_viewer/mask_overlay_geometry.hpp"

/**
 * @file
 * @brief Viewport presentation of the AI Mask tool input (SM9): the prompt markers of the open
 *        prompt group, the stroke or box that the pointer draws, the outline of the selected
 *        people, and the tool cursor. Item/logical coordinates; GUI-thread pure functions.
 *
 * The coverage of the Mask is not drawn here: the viewport composite shows it (SM8).
 */
namespace alcedo {

/// The cursor of the selected AI Mask prompt tool.
enum class AiMaskInputCursorKind : std::uint8_t { None, Point, Box, Path };

/// A click prompt: a disc with a plus (Add) or a minus (Subtract).
struct AiMaskInputPointMarker {
  QPointF item{};
  bool    add = true;
};

/// A Path prompt: the stroke through its points.
struct AiMaskInputStroke {
  std::vector<QPointF> item;
  bool                 add = true;
};

/// A Box prompt; the corners can form a rotated rectangle on a rotated view.
struct AiMaskInputBox {
  std::array<QPointF, 4> corners{};
  bool                   add = true;
};

/**
 * @brief The AI Mask input overlay of one frame.
 *
 * @p add of a marker is the effect of the input on the Mask: an Add point in a Subtract group
 * removes, so the tool sets it to false. @p photo_rect clips everything except the cursor; an
 * empty rectangle disables clipping.
 */
struct AiMaskInputOverlayDisplay {
  std::vector<AiMaskInputPointMarker> points;
  std::vector<AiMaskInputStroke>      strokes;
  std::vector<AiMaskInputBox>         boxes;
  /// Closed outlines of the union of the selected person boxes.
  std::vector<std::vector<QPointF>>   person_outlines;
  /// The person box under the pointer in the People list, dashed; empty when there is none.
  std::vector<QPointF>                highlighted_person;
  AiMaskInputCursorKind               cursor = AiMaskInputCursorKind::None;
  QPointF                             cursor_item{};
  /// Sign of the cursor badge: the Add or Subtract mode of the tool.
  bool                                cursor_add  = true;
  /// A model request runs: the cursor shows in the inactive color without a badge.
  bool                                cursor_busy = false;
  QRectF                              photo_rect{};
};

/// Colors and logical sizes of the AI Mask input overlay (AppTheme `aiMaskInput*` roles).
struct AiMaskInputOverlayStyle {
  QColor add_color       = QColor(0x30, 0xA4, 0x6C);
  QColor subtract_color  = QColor(0xE5, 0x48, 0x4D);
  QColor line_color      = QColor(0xE0, 0xE0, 0xE0);
  QColor outline_color   = QColor(0x0C, 0x0C, 0x0C);
  QColor inactive_color  = QColor(0x7A, 0x7A, 0x7A);
  /// Radius of a point marker disc.
  float  marker_radius   = 7.0f;
  /// Dark rim around markers, strokes, boxes, and outlines.
  float  outline_width   = 1.5f;
  /// Core width of a Path stroke.
  float  stroke_width    = 4.0f;
  /// Core width of a box and of the person outline.
  float  frame_width     = 2.0f;
  float  antialias_width = 1.0f;
};

/**
 * @brief Item-space outlines of the union of normalized analysis image boxes.
 *
 * @p normalized_boxes are analysis image boxes in [0, 1]; @p analysis_to_item maps them to the
 * item (@ref AiMaskAnalysisToItemTransform). Overlapping boxes give one outline; separate boxes
 * give one outline each; a box inside another adds nothing.
 */
[[nodiscard]] auto AiMaskPersonUnionOutlines(const std::vector<QRectF>& normalized_boxes,
                                             const QTransform&          analysis_to_item)
    -> std::vector<std::vector<QPointF>>;

/**
 * @brief Triangle list of @p display in painter order: person outlines, boxes, strokes, point
 *        markers, then the cursor. Every element has a dark rim under a light or colored core.
 */
[[nodiscard]] auto BuildAiMaskInputOverlayTriangles(const AiMaskInputOverlayDisplay& display,
                                                    const AiMaskInputOverlayStyle&   style)
    -> std::vector<MaskOverlayVertex>;

}  // namespace alcedo
