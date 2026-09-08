//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <span>

#include "edit/geometry/types.hpp"
#include "edit/mask/mask_asset.hpp"

namespace alcedo {

/// First parameterized Brush source JSON identity. Raster-only documents still
/// serialize without this key until the NM7.3 persistence cutover.
inline constexpr std::uint32_t kBrushSourceFormatVersion = 1;
/// Host dab/paint/erase algorithm identity. Replay must reject any other value.
inline constexpr std::uint32_t kBrushRasterAlgorithmVersion = 1;
/// Project Mix-cache container identity. Distinct from @ref kMaskAssetFormatVersion.
inline constexpr std::uint32_t kProjectMaskCacheFormatVersion = 1;
/// Maximum spacing between canonical dabs, as a fraction of the local sample radius.
inline constexpr float kBrushDabSpacingRadiusFraction = 0.25f;

enum class BrushStrokeMode : std::uint8_t {
  Paint = 0,
  Erase = 1,
};

/**
 * @brief One canonical Brush sample in untranslated reference-pixel coordinates.
 *
 * Values are IEEE-754 binary32. @p local_x / @p local_y are stored after subtracting
 * @c placement_translation. @p radius is the dab radius in reference pixels.
 * @p strength and @p hardness stay in `[0, 1]`.
 */
struct BrushCanonicalSample {
  float local_x  = 0.0f;
  float local_y  = 0.0f;
  float radius   = 1.0f;
  float strength = 1.0f;
  float hardness = 1.0f;

  friend auto operator==(const BrushCanonicalSample&, const BrushCanonicalSample&) -> bool = default;
};

/**
 * @brief Reject NaN/Inf, non-positive radius, or strength/hardness outside `[0, 1]`.
 *
 * @param sample Candidate sample. Not mutated.
 * @throws std::invalid_argument when a field fails the encoding rules.
 */
void ValidateBrushCanonicalSample(const BrushCanonicalSample& sample);

/**
 * @brief Quantize coverage in `[0, 1]` to packed R8 with round-half-up.
 *
 * Matches the native evaluator: `clamp(coverage * 255 + 0.5, 0, 255)`.
 *
 * @param coverage Finite coverage. Non-finite values throw.
 * @return Byte in `[0, 255]`.
 * @throws std::invalid_argument when @p coverage is not finite.
 */
[[nodiscard]] auto QuantizeMaskCoverageToR8(float coverage) -> std::uint8_t;

/**
 * @brief Decode one packed R8 sample to coverage in `[0, 1]`.
 */
[[nodiscard]] inline auto CoverageFromMaskR8(std::uint8_t value) -> float {
  return static_cast<float>(value) / 255.0f;
}

/**
 * @brief Unit dab coverage at @p distance from a sample center, before R8 quantization.
 *
 * Hardness `1` is a hard disk of radius @p radius. Otherwise coverage is 1 inside
 * `hardness * radius`, 0 at and beyond @p radius, and linear between. The result is
 * multiplied by @p strength. Distance and radius use the reference-pixel metric.
 *
 * @throws std::invalid_argument when radius, strength, or hardness fail
 *         @ref ValidateBrushCanonicalSample, or when @p distance is negative or not finite.
 */
[[nodiscard]] auto BrushDabCoverage(float distance, float radius, float strength, float hardness)
    -> float;

/**
 * @brief Canonical Brush raster extent covering full ReferenceSpace.
 *
 * Uses the same ceil-scale rule as LLF mask dimensions, but the long-edge cap is
 * @ref kMaximumRasterMaskAxis (4096), not the LLF 2048 cap. Zoom, DPR, and Interactive
 * output size must not be passed here.
 *
 * @param full_reference Developed full-frame extent in reference pixels.
 * @return Positive extent with each axis in `[1, 4096]`.
 * @throws std::invalid_argument when @p full_reference is empty.
 */
[[nodiscard]] auto CanonicalBrushRasterExtent(Extent2D full_reference) -> Extent2D;

/**
 * @brief Full-frame bounds stored with a parameterized Brush canonical raster.
 */
[[nodiscard]] inline auto CanonicalBrushReferenceBounds() -> NormalizedRect { return {}; }

/**
 * @brief Reference-pixel center of canonical raster texel `@p x, @p y`.
 *
 * Mapping is `(x + 0.5) * full_w / raster_w`. Pointer samples do not receive this
 * half-texel offset; only raster/evaluator tests do.
 *
 * @throws std::invalid_argument when extents are empty or the texel is outside the raster.
 */
[[nodiscard]] auto CanonicalBrushTexelReferenceCenter(std::uint32_t x, std::uint32_t y,
                                                      Extent2D raster, Extent2D full_reference)
    -> Vector2;

/**
 * @brief Convert Brush source feather into source-texel radius for signed-distance feather.
 *
 * Matches the native Mask pass:
 * `radius_texels = feather_radius * 0.5 * (x_scale + y_scale)` with
 * `x_scale = raster_w / (full_w * max(bounds.w, 1e-6))`.
 *
 * @p feather_radius is the existing Brush source field, in the reference-pixel metric
 * (equal to source texels when the raster matches full reference and bounds are full-frame).
 *
 * @throws std::invalid_argument when extents are empty or @p feather_radius is not finite
 *         and nonnegative, or bounds components are not finite.
 */
[[nodiscard]] auto BrushFeatherRadiusToSourceTexels(float feather_radius, Extent2D raster,
                                                    NormalizedRect bounds, Extent2D full_reference)
    -> float;

/**
 * @brief Bilinear sample of tightly packed R8 at normalized UV, matching the native Mask pass.
 *
 * UV outside `[0, 1]` returns 0. Pixel centers use `u * width - 0.5`. The result is
 * coverage in `[0, 1]`.
 *
 * @throws std::invalid_argument when @p extent is empty or @p pixels is not tightly packed.
 */
[[nodiscard]] auto SamplePackedR8Bilinear(std::span<const std::uint8_t> pixels, Extent2D extent,
                                          float u, float v) -> float;

/**
 * @brief Paint combine on packed R8: `max(previous, dab)`.
 */
[[nodiscard]] inline auto PaintBrushR8(std::uint8_t previous, std::uint8_t dab) -> std::uint8_t {
  return previous > dab ? previous : dab;
}

/**
 * @brief Erase combine on packed R8: `min(previous, 255 - dab)`.
 */
[[nodiscard]] inline auto EraseBrushR8(std::uint8_t previous, std::uint8_t dab) -> std::uint8_t {
  const auto inverted = static_cast<std::uint8_t>(255u - dab);
  return previous < inverted ? previous : inverted;
}

}  // namespace alcedo
