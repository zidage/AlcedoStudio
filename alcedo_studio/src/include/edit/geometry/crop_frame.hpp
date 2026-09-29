//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>

#include "edit/geometry/types.hpp"

namespace alcedo {

/**
 * @brief Wraps @p degrees into [-180, 180]. Non-finite input returns 0.
 */
[[nodiscard]] auto NormalizeRotationDegrees(float degrees) -> float;

/**
 * @brief Constrains a document crop frame so every rotated corner stays inside the source.
 *
 * The crop frame is an axis-aligned rectangle in the output. Its center is
 * `(x + w / 2, y + h / 2)` in source-normalized coordinates, its size is `w * W` by `h * H`
 * source pixels, and the source is rotated by @p rotation_degrees about that center. With a
 * rotation the unrotated rectangle of the same center and size may leave [0, 1], so `x` and `y`
 * may be negative; the center and the rotated corners are what this function constrains.
 *
 * When the rotated frame does not fit, its size is reduced with the aspect kept, and the center
 * is then moved into the range where all four rotated corners lie inside `[0, W] x [0, H]`.
 * A frame that already fits is returned unchanged.
 *
 * @throws std::runtime_error if a component of @p crop is not finite or @p source is empty.
 */
[[nodiscard]] auto ClampCropToRotatedSource(NormalizedRect crop, float rotation_degrees,
                                            Extent2D source) -> NormalizedRect;

/**
 * @brief Corners of the crop frame in source reference pixels, clockwise from the output's
 *        top-left corner. Uses the same rotation as ResolveRenderGeometry.
 */
[[nodiscard]] auto CropFrameCornersInReference(NormalizedRect crop, float rotation_degrees,
                                               Extent2D source) -> std::array<Vector2, 4>;

}  // namespace alcedo
