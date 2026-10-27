//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>

namespace alcedo {

/// OkLab lightness of the Hue Range bar colors (Mask ranges MR4).
inline constexpr float kMaskHueBarLightness = 0.75f;
/// OkLCh chroma of the Hue Range bar colors (Mask ranges MR4).
inline constexpr float kMaskHueBarChroma    = 0.12f;

/**
 * @brief Display color of hue @p hue_degrees on the Hue Range bar of a Mask (Mask ranges MR4).
 *
 * The color is OkLCh (@ref kMaskHueBarLightness, @ref kMaskHueBarChroma, @p hue_degrees). It goes
 * to linear AP1 through the inverse of the OkLab transform that the MaskEvaluate kernels use
 * (`LinearAp1ToOkLab`, AP1 neutrals have a = b = 0), then to linear Rec.709 (ACES AP1 to sRGB with
 * the Bradford D60 to D65 adaptation), is clipped to [0, 1], and is encoded with the sRGB transfer
 * function. So a position on the bar shows the hue that the kernel tests for that position;
 * clipping can move the hue of a color outside the sRGB gamut.
 *
 * @return Encoded sRGB red, green, blue in [0, 1].
 */
[[nodiscard]] auto     MaskHueBarColor(float hue_degrees) -> std::array<float, 3>;

/**
 * @brief Display gray of OkLab lightness @p lightness on the Luminance Range bar of a Mask.
 *
 * A neutral with OkLab L = @p lightness has linear luminance L^3 in AP1 and in Rec.709; the
 * result is that value clipped to [0, 1] and encoded with the sRGB transfer function. So a
 * position on the bar shows the lightness that the kernel tests for that position.
 */
[[nodiscard]] auto     MaskLuminanceBarGray(float lightness) -> float;

}  // namespace alcedo
