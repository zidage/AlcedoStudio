//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QColor>
#include <QPointF>
#include <QRectF>
#include <optional>
#include <utility>
#include <vector>

#include "ui/edit_viewer/mask_overlay_geometry.hpp"

/**
 * @file
 * @brief Antialiased triangle-list primitives of the QSG viewport overlays.
 *
 * Every primitive appends premultiplied @ref MaskOverlayVertex triangles in item/logical
 * coordinates. An antialias width @p aa > 0 adds an edge fringe whose alpha falls to zero. A
 * non-empty @p clip rectangle drops geometry outside it; an empty rectangle disables clipping.
 * Non-finite points add no triangles. GUI-thread, pure functions.
 */
namespace alcedo::overlay_triangles {

/// Segments shorter than this are degenerate.
inline constexpr float kOverlayMinLength = 1.0e-6f;

[[nodiscard]] auto     IsFinitePoint(const QPointF& point) -> bool;
[[nodiscard]] auto     Premultiply(const QColor& color, float alpha_scale) -> MaskOverlayVertex;
[[nodiscard]] auto     MakeVertex(const QPointF& point, const QColor& color, float alpha_scale)
    -> MaskOverlayVertex;
void AppendTriangle(std::vector<MaskOverlayVertex>& triangles, const QPointF& a, const QPointF& b,
                    const QPointF& c, const QColor& color, float alpha_a, float alpha_b,
                    float alpha_c);
void AppendTriangle(std::vector<MaskOverlayVertex>& triangles, const QPointF& a, const QPointF& b,
                    const QPointF& c, const QColor& color);
[[nodiscard]] auto ExpandedClip(const QRectF& clip, float pad) -> QRectF;
[[nodiscard]] auto PointInsideClip(const QPointF& point, const QRectF& clip) -> bool;
/// Liang-Barsky clip of the segment @p a to @p b against @p clip.
[[nodiscard]] auto ClipSegment(const QPointF& a, const QPointF& b, const QRectF& clip)
    -> std::optional<std::pair<QPointF, QPointF>>;
[[nodiscard]] auto PerpUnit(const QPointF& a, const QPointF& b) -> QPointF;
void AppendStrokeCoreAndFringe(std::vector<MaskOverlayVertex>& triangles, const QPointF& a,
                               const QPointF& b, float width, float aa, const QColor& color);
void AppendRoundCap(std::vector<MaskOverlayVertex>& triangles, const QPointF& center,
                    const QPointF& along, float radius, float aa, const QColor& color);
void AppendClippedStroke(std::vector<MaskOverlayVertex>& triangles, const QPointF& a,
                         const QPointF& b, float width, float aa, const QColor& color,
                         const QRectF& clip, bool round_caps);
void AppendPolylineStroke(std::vector<MaskOverlayVertex>& triangles,
                          const std::vector<QPointF>& points, bool closed, float width, float aa,
                          const QColor& color, const QRectF& clip);
/// Dashes follow the polyline arc length, so the dash phase stays continuous across vertices.
void AppendDashedPolylineStroke(std::vector<MaskOverlayVertex>& triangles,
                                const std::vector<QPointF>& points, bool closed, float width,
                                float aa, const QColor& color, const QRectF& clip, float dash_len,
                                float gap_len);
void AppendFilledDisc(std::vector<MaskOverlayVertex>& triangles, const QPointF& center,
                      float radius, float aa, const QColor& color, const QRectF& clip);
void AppendRing(std::vector<MaskOverlayVertex>& triangles, const QPointF& center, float inner_r,
                float outer_r, float aa, const QColor& color, const QRectF& clip);
void AppendHollowCircle(std::vector<MaskOverlayVertex>& triangles, const QPointF& center,
                        float radius, float width, float aa, const QColor& color,
                        const QRectF& clip);
void AppendDashedHollowCircle(std::vector<MaskOverlayVertex>& triangles, const QPointF& center,
                              float radius, float width, float aa, const QColor& color,
                              const QRectF& clip, float dash_len, float gap_len);

}  // namespace alcedo::overlay_triangles
