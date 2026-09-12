//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <optional>

#include "edit/geometry/types.hpp"
#include "edit/mask/mask_model.hpp"

namespace alcedo {

/// Evaluator epsilon shared with host Mix / native analytic shaders.
inline constexpr float kAnalyticMaskEpsilon = 1.0e-6f;
/// Minimum positive Radial radii before a center-out drag becomes a Mask.
inline constexpr float kAnalyticCreationMinRadius = 1.0e-5f;
/// Default inner feather for center-out Radial creation. The full-strength
/// boundary starts at rho = 1 - kAnalyticCreationDefaultInnerFeather, so a new
/// Radial Mask fades out over the outer half of its radii instead of a hard edge.
inline constexpr float kAnalyticCreationDefaultInnerFeather = 0.5f;

/**
 * @brief Finite analytic handle identity. Not a per-texel or per-dab proxy.
 *
 * Overlay hit testing maps pointer discs onto these ids. Creation drags do not
 * use a handle: Radial is center-out from the press, Linear uses two endpoints.
 */
enum class AnalyticMaskHandle : std::uint8_t {
  None                 = 0,
  RadialCenter         = 1,
  RadialMajor          = 2,
  RadialMinor          = 3,
  RadialRotate         = 4,
  RadialInnerFeather   = 5,
  RadialOuterFeather   = 6,
  LinearOrigin         = 7,
  LinearDirection      = 8,
  LinearStartBoundary  = 9,
  LinearEndBoundary    = 10,
};

/**
 * @brief Rotated local coordinates of @p normalized relative to @p source.
 *
 * @p local.x = cos(r)*dx + sin(r)*dy, @p local.y = -sin(r)*dx + cos(r)*dy.
 * These are @c u * major_radius and @c v * minor_radius in the native evaluator.
 * Does not divide by radii, so axis identity stays stable when a numeric radius
 * crosses the other axis.
 *
 * @return Empty when any input is not finite.
 */
[[nodiscard]] auto RadialLocalAxes(const RadialMaskSource& source, Vector2 normalized)
    -> std::optional<Vector2>;

/**
 * @brief Evaluator rho of @p normalized for @p source.
 *
 * @return Empty when radii or coordinates are not finite.
 */
[[nodiscard]] auto RadialRho(const RadialMaskSource& source, Vector2 normalized)
    -> std::optional<float>;

/**
 * @brief True when both Radial radii exceed @ref kAnalyticCreationMinRadius.
 */
[[nodiscard]] auto RadialCreationIsValid(const RadialMaskSource& source) -> bool;

/**
 * @brief True when the Linear normal has length above the source validator minimum.
 */
[[nodiscard]] auto LinearCreationIsValid(const LinearGradientMaskSource& source) -> bool;

/**
 * @brief Center-out Radial: press is the center, drag sets positive x/y radii.
 *
 * Rotation and outer feather stay 0. Inner feather defaults to
 * @ref kAnalyticCreationDefaultInnerFeather so a new Radial Mask is feathered,
 * not hard-edged. Radii are absolute normalized deltas, never swapped.
 */
[[nodiscard]] auto RadialFromCenterOut(Vector2 center_normalized, Vector2 current_normalized)
    -> RadialMaskSource;

/**
 * @brief Linear from press @p a to current @p b in normalized ReferenceSpace.
 *
 * @c origin = (a+b)/2, @c normal = normalize(b-a), @c transition_distance = |b-a|,
 * @c start_value = 1, @c end_value = 0. Degenerate @p a==@p b yields a zero normal.
 */
[[nodiscard]] auto LinearFromEndpoints(Vector2 a_normalized, Vector2 b_normalized)
    -> LinearGradientMaskSource;

/**
 * @brief Move Radial center. Radii, rotation, and feathers are unchanged.
 */
[[nodiscard]] auto TranslateRadialCenter(RadialMaskSource source, Vector2 center_normalized)
    -> RadialMaskSource;

/**
 * @brief Move Linear origin. Direction, width, and end values are unchanged.
 */
[[nodiscard]] auto TranslateLinearOrigin(LinearGradientMaskSource source, Vector2 origin_normalized)
    -> LinearGradientMaskSource;

/**
 * @brief Set major_radius from the local x-axis component of @p normalized.
 *
 * Uses absolute local x so dragging across the center does not swap axis identity
 * with the minor handle. Minor radius, rotation, center, and feathers stay fixed.
 *
 * @return Empty when the local frame cannot be formed.
 */
[[nodiscard]] auto UpdateRadialMajorRadius(const RadialMaskSource& source, Vector2 normalized)
    -> std::optional<RadialMaskSource>;

/**
 * @brief Set minor_radius from the local y-axis component of @p normalized.
 */
[[nodiscard]] auto UpdateRadialMinorRadius(const RadialMaskSource& source, Vector2 normalized)
    -> std::optional<RadialMaskSource>;

/**
 * @brief Set rotation from the normalized-space angle of @p normalized about center.
 *
 * @p unwrapped_rotation is the continuous angle from the previous sample. It is
 * updated in place so wrapping across ±π does not jump by 2π.
 *
 * @return Empty when @p normalized is not finite or coincides with the center.
 */
[[nodiscard]] auto UpdateRadialRotation(const RadialMaskSource& source, Vector2 normalized,
                                        float& unwrapped_rotation)
    -> std::optional<RadialMaskSource>;

/**
 * @brief Set inner_feather from evaluator rho at @p normalized. Radii stay fixed.
 *
 * @c inner_feather = clamp(1 - rho, 0, 1).
 */
[[nodiscard]] auto UpdateRadialInnerFeather(const RadialMaskSource& source, Vector2 normalized)
    -> std::optional<RadialMaskSource>;

/**
 * @brief Set outer_feather from evaluator rho at @p normalized. Radii stay fixed.
 *
 * @c outer_feather = max(0, rho - 1).
 */
[[nodiscard]] auto UpdateRadialOuterFeather(const RadialMaskSource& source, Vector2 normalized)
    -> std::optional<RadialMaskSource>;

/**
 * @brief Set Linear normal to the direction from origin to @p normalized.
 *
 * Origin and transition_distance stay fixed. Empty when the direction is degenerate.
 */
[[nodiscard]] auto UpdateLinearDirection(const LinearGradientMaskSource& source,
                                         Vector2 normalized)
    -> std::optional<LinearGradientMaskSource>;

/**
 * @brief Set transition_distance to twice the projected distance from origin.
 *
 * Origin and normal stay fixed. Both start and end boundaries stay symmetric.
 * Empty when the stored normal is degenerate.
 */
[[nodiscard]] auto UpdateLinearTransitionFromBoundary(const LinearGradientMaskSource& source,
                                                      Vector2 normalized)
    -> std::optional<LinearGradientMaskSource>;

/**
 * @brief Apply one existing-mask handle drag in normalized ReferenceSpace.
 *
 * Creation drags must use @ref RadialFromCenterOut or @ref LinearFromEndpoints.
 *
 * @param handle Selected control. @c None is rejected.
 * @param source Current source. Kind must match @p handle.
 * @param normalized Pointer in normalized ReferenceSpace. Off-image values are allowed.
 * @param unwrapped_rotation In/out continuous rotation for @c RadialRotate.
 * @return Updated source, or empty when the handle/kind/sample is invalid.
 */
[[nodiscard]] auto ApplyAnalyticMaskHandle(AnalyticMaskHandle handle, const MaskSource& source,
                                           Vector2 normalized, float& unwrapped_rotation)
    -> std::optional<MaskSource>;

}  // namespace alcedo
