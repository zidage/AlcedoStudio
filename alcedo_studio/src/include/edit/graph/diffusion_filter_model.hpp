//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <algorithm>

namespace alcedo {

/// Stored diffusion filter strength range. The document stores only this value.
inline constexpr float kDiffusionStrengthMin = 0.0f;
inline constexpr float kDiffusionStrengthMax = 1.0f;

/**
 * @brief Optical shape of the simulated black diffusion filter for one stored strength.
 *
 * The document stores only `strength`. This Model resolves every internal parameter from it,
 * so a change to the shape constants changes the rendering of existing documents without a
 * document migration. Renderers must bump `kDrtImplementationVersion` when they change.
 *
 * - @ref scatter_fraction: share of the light that the filter scatters (`s`).
 * - @ref glow_radius: widest scatter sigma as a fraction of the image short side.
 * - @ref base_sigma_fraction: narrowest scatter sigma as a fraction of the image short side.
 * - @ref power_law_exponent: exponent `p` of the `r^-p` point spread tail.
 * - @ref black_mist: veil reduction of the widest scatter levels and light absorption.
 * - @ref black_absorption: transmission loss per unit of `black_mist * s`.
 * - @ref highlight_glow: gain of the highlight boost in the scatter branch.
 * - @ref highlight_low_stops: log2 scene-linear value where the highlight boost starts.
 * - @ref highlight_high_stops: log2 scene-linear value where the highlight boost is full.
 *
 * The boost follows a smoothstep in log2 exposure across several stops. A narrow linear knee
 * makes the output a steep function of the input at the knee, which draws a hard contour
 * through every smooth gradient that crosses it.
 */
struct DiffusionFilterShape {
  float scatter_fraction    = 0.0f;
  float glow_radius         = 0.12f;
  float base_sigma_fraction = 0.002f;
  float power_law_exponent  = 2.6f;
  float black_mist          = 0.5f;
  float black_absorption    = 0.1f;
  float highlight_glow       = 6.0f;
  float highlight_low_stops  = -1.5f;
  float highlight_high_stops = 1.5f;
};

/// Scatter fraction at the maximum stored strength. Strength 1 equals the 0.4 strength of the
/// first calibration (0.4 * 0.4).
inline constexpr float kDiffusionMaxScatterFraction = 0.16f;

/**
 * @brief Resolve the filter shape for a stored strength.
 *
 * Strength scales only the scatter fraction. The glow profile, black mist, and highlight boost
 * are fixed properties of the simulated filter.
 */
[[nodiscard]] inline auto ResolveDiffusionFilterShape(float strength) -> DiffusionFilterShape {
  DiffusionFilterShape shape;
  shape.scatter_fraction =
      kDiffusionMaxScatterFraction *
      std::clamp(strength, kDiffusionStrengthMin, kDiffusionStrengthMax);
  return shape;
}

/// True when @p strength changes pixels. Strength 0 only decodes the scene to linear.
[[nodiscard]] inline auto IsDiffusionFilterActive(float strength) -> bool {
  return strength > kDiffusionStrengthMin;
}

}  // namespace alcedo
