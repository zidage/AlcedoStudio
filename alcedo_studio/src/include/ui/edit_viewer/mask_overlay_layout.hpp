//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <vector>

#include <QPointF>
#include <QRectF>

#include "edit/geometry/types.hpp"
#include "edit/mask/mask_model.hpp"
#include "ui/edit_viewer/mask_edit_geometry.hpp"
#include "ui/edit_viewer/mask_overlay_geometry.hpp"

namespace alcedo {

/**
 * @brief Map a normalized ReferenceSpace point to item coordinates.
 *
 * @return Empty when @p mapping is invalid or the mapped point is not finite.
 */
[[nodiscard]] auto MapNormalizedMaskPointToItem(const MaskEditViewMapping& mapping,
                                                Vector2 normalized) -> std::optional<QPointF>;

/**
 * @brief Normalized Radial boundary point at @p rho and angle @p theta_radians.
 *
 * Inverse of the native Radial evaluator's local (u, v) frame:
 * @c u = rho * cos(theta), @c v = rho * sin(theta).
 */
[[nodiscard]] auto RadialNormalizedPoint(const RadialMaskSource& source, float rho,
                                         float theta_radians) -> Vector2;

/**
 * @brief Tessellate a Radial iso-@p rho curve into item-space vertices.
 *
 * Subdivides until projected chord error is at most
 * @ref kMaskOverlayMaxChordDeviationLogicalPx or @ref kMaskOverlayMaxEllipseVertices
 * is reached. Degenerate or unmappable curves return an empty polyline.
 *
 * @param mapping Current viewer mapping.
 * @param source Radial parameters in normalized ReferenceSpace.
 * @param rho Evaluator rho (1 = radius, inner/outer feather boundaries differ).
 * @param clip Drop vertices whose item position lies outside @p clip expanded by
 *        one handle radius. Empty @p clip disables clipping.
 */
[[nodiscard]] auto TessellateRadialBoundaryItemPolyline(const MaskEditViewMapping& mapping,
                                                        const RadialMaskSource& source, float rho,
                                                        const QRectF& clip) -> std::vector<QPointF>;

/**
 * @brief Existing Brush: Move handle at @p placement_translation. No stroke path.
 */
[[nodiscard]] auto MakeBrushExistingOverlayDisplay(const MaskEditViewMapping& mapping,
                                                   Vector2 placement_translation,
                                                   const QRectF& clip) -> MaskOverlayDisplay;

/**
 * @brief Existing Radial: center, axes, rotation, and distinct feather handles.
 *
 * Connectors only. No ellipse fill and no iso-rho outline.
 */
[[nodiscard]] auto MakeRadialExistingOverlayDisplay(const MaskEditViewMapping& mapping,
                                                    const RadialMaskSource& source,
                                                    const MaskOverlayStyle& style,
                                                    const QRectF& clip) -> MaskOverlayDisplay;

/**
 * @brief Existing Linear Gradient: origin, direction, and transition boundaries.
 *
 * Finite connectors only. No filled coverage band.
 */
[[nodiscard]] auto MakeLinearExistingOverlayDisplay(const MaskEditViewMapping& mapping,
                                                    const LinearGradientMaskSource& source,
                                                    const MaskOverlayStyle& style,
                                                    const QRectF& clip) -> MaskOverlayDisplay;

/**
 * @brief Initial Brush drawing: cursor plus a temporary path through @p item_path.
 *
 * @p item_path must already be the canonical sample sequence mapped to item space.
 * This function does not resample pointer events. No area fill.
 */
[[nodiscard]] auto MakeBrushCreatingOverlayDisplay(const std::vector<QPointF>& item_path,
                                                   QPointF cursor_item,
                                                   float cursor_radius_logical_px,
                                                   const QRectF& clip) -> MaskOverlayDisplay;

/**
 * @brief Initial Radial drawing: existing handles plus a temporary outline at rho = 1.
 */
[[nodiscard]] auto MakeRadialCreatingOverlayDisplay(const MaskEditViewMapping& mapping,
                                                    const RadialMaskSource& source,
                                                    const MaskOverlayStyle& style,
                                                    const QRectF& clip) -> MaskOverlayDisplay;

/**
 * @brief Initial Linear drawing: existing handles plus finite locus guides.
 */
[[nodiscard]] auto MakeLinearCreatingOverlayDisplay(const MaskEditViewMapping& mapping,
                                                    const LinearGradientMaskSource& source,
                                                    const MaskOverlayStyle& style,
                                                    const QRectF& clip) -> MaskOverlayDisplay;

/**
 * @brief Item-space length of a ReferenceSpace radius around @p center_reference.
 *
 * Maps @p center_reference and a point one @p radius_reference_px along +X.
 * Empty when either map fails.
 */
[[nodiscard]] auto MapReferenceRadiusToItem(const MaskEditViewMapping& mapping,
                                            Vector2 center_reference, float radius_reference_px)
    -> std::optional<float>;

}  // namespace alcedo
