//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QPointF>
#include <QRectF>
#include <vector>

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
 * @brief Map the item-space perpendicular foot on one Linear Gradient locus.
 *
 * The evaluator normal is a covector under a non-square photograph transform.
 * This helper keeps the visible control axis perpendicular to the mapped guide
 * while the returned point remains on the exact normalized-space locus.
 */
[[nodiscard]] auto MapLinearLocusNormalPointToItem(const MaskEditViewMapping&      mapping,
                                                   const LinearGradientMaskSource& source,
                                                   float signed_distance) -> std::optional<QPointF>;

/**
 * @brief Convert an item-space direction control point to an evaluator normal sample.
 *
 * This is the inverse interaction mapping for
 * @ref MapLinearLocusNormalPointToItem. The returned normalized point is one
 * unit normal from the source origin and can be passed to
 * @c UpdateLinearDirection.
 */
[[nodiscard]] auto MapItemPointToLinearDirectionSample(const MaskEditViewMapping&      mapping,
                                                       const LinearGradientMaskSource& source,
                                                       QPointF item) -> std::optional<Vector2>;

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
                                                   Vector2                    placement_translation,
                                                   const QRectF& clip) -> MaskOverlayDisplay;

/**
 * @brief Existing Radial: center, axes, rotation, feather handles, and iso-rho lines.
 *
 * Shows the base ellipse and distinct inner/outer feather contours. Coincident
 * rhos are drawn once. No ellipse fill.
 */
[[nodiscard]] auto MakeRadialExistingOverlayDisplay(const MaskEditViewMapping& mapping,
                                                    const RadialMaskSource&    source,
                                                    const MaskOverlayStyle&    style,
                                                    const QRectF& clip) -> MaskOverlayDisplay;

/**
 * @brief Existing Linear Gradient: origin, direction, and three parallel loci.
 *
 * Guides are clipped to the photograph with Geometry crop-style dual strokes
 * and short edge grips. Ends are not joined into a kite or closed polygon.
 */
[[nodiscard]] auto MakeLinearExistingOverlayDisplay(const MaskEditViewMapping&      mapping,
                                                    const LinearGradientMaskSource& source,
                                                    const MaskOverlayStyle&         style,
                                                    const QRectF& clip) -> MaskOverlayDisplay;

/**
 * @brief Initial Brush drawing: cursor plus a temporary path through @p item_path.
 *
 * @p item_path must already be the canonical sample sequence mapped to item space.
 * This function does not resample pointer events. No area fill.
 */
[[nodiscard]] auto MakeBrushCreatingOverlayDisplay(const std::vector<QPointF>& item_path,
                                                   QPointF                     cursor_item,
                                                   float         cursor_radius_logical_px,
                                                   const QRectF& clip) -> MaskOverlayDisplay;

/**
 * @brief Initial Radial drawing: selected contours and handles while the drag is open.
 */
[[nodiscard]] auto MakeRadialCreatingOverlayDisplay(const MaskEditViewMapping& mapping,
                                                    const RadialMaskSource&    source,
                                                    const MaskOverlayStyle&    style,
                                                    const QRectF& clip) -> MaskOverlayDisplay;

/**
 * @brief Initial Linear drawing: three photograph-clipped loci plus handles.
 */
[[nodiscard]] auto MakeLinearCreatingOverlayDisplay(const MaskEditViewMapping&      mapping,
                                                    const LinearGradientMaskSource& source,
                                                    const MaskOverlayStyle&         style,
                                                    const QRectF& clip) -> MaskOverlayDisplay;

[[nodiscard]] auto HitTestMaskOverlayHandle(const MaskOverlayDisplay& display, QPointF item,
                                            float hit_radius_logical_px) -> MaskOverlayHandleId;

/**
 * @brief Axis-aligned photograph rectangle in item coordinates.
 *
 * Maps photograph UV (0,0) and (1,1) through the current view. Empty when mapping
 * is invalid.
 */
[[nodiscard]] auto PhotographItemRect(const MaskEditViewMapping& mapping) -> QRectF;

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
