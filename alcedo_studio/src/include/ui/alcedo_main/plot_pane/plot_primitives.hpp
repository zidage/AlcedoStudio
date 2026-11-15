//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QColor>
#include <QPointF>
#include <QRectF>
#include <array>
#include <vector>

#include "ui/edit_viewer/mask_overlay_geometry.hpp"

/**
 * @file
 * @brief Antialiased triangle primitives of the plot layers, on top of the overlay primitives.
 *
 * Widths and radii are the visible size in item pixels: a stroke of width 1.25 covers 1.25 pixels
 * across, with its antialiased edge included.
 */
namespace alcedo::ui::plot_primitives {

/// Width of the antialiased edge of every plot primitive, in item pixels.
inline constexpr float kAntialiasWidth = 1.0f;

/// Solid polyline. @p clip limits the drawing (an empty rectangle draws everywhere).
void AppendStroke(std::vector<MaskOverlayVertex>& triangles, const std::vector<QPointF>& points,
                  float width, const QColor& color, const QRectF& clip);
/// Dashed polyline; the dash phase follows the arc length.
void AppendDashedStroke(std::vector<MaskOverlayVertex>& triangles,
                        const std::vector<QPointF>& points, float width, float dash_length,
                        float gap_length, const QColor& color, const QRectF& clip);
/// Ring whose outer edge is at @p radius, @p width wide, antialiased on both edges.
void AppendRing(std::vector<MaskOverlayVertex>& triangles, const QPointF& center, float radius,
                float width, const QColor& color, const QRectF& clip);
/// Filled disc whose edge is at @p radius.
void AppendDisc(std::vector<MaskOverlayVertex>& triangles, const QPointF& center, float radius,
                const QColor& color, const QRectF& clip);
/// Opaque or translucent rectangle with no antialiasing (axis-aligned fills).
void AppendRect(std::vector<MaskOverlayVertex>& triangles, const QRectF& rect, const QColor& color);

/// Hue of @p tint with the smallest component at 0 and the largest at 1. A neutral tint gives
/// white.
[[nodiscard]] auto TintChroma(const QColor& tint) -> std::array<float, 3>;
/// Chroma of three channel tints, scaled per component so that the three add up to white.
[[nodiscard]] auto AdditiveChannelChroma(const std::array<QColor, 3>& tints)
    -> std::array<std::array<float, 3>, 3>;
/// Opaque color of @p chroma at @p strength.
[[nodiscard]] auto ScaledChromaColor(const std::array<float, 3>& chroma, float strength) -> QColor;
/// Vertex that adds @p color to the target: premultiplied color with alpha 0, so the
/// premultiplied blending of the scene graph adds it (source + target).
[[nodiscard]] auto AdditiveVertex(const QPointF& point, const QColor& color, float scale)
    -> MaskOverlayVertex;

}  // namespace alcedo::ui::plot_primitives
