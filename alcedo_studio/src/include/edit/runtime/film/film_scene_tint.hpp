//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <span>

namespace alcedo {

/**
 * @brief The white of the RAW decode: the light of scene light As Shot of the film print.
 *
 * Develop mode Custom gives its Kelvin and tint; the other modes give the solved as-shot values.
 * @ref xy holds the tint, so it is the chromaticity of the light of the shot.
 */
struct FilmRawWhite {
  double                cct = 0.0;  ///< Colour temperature, K. It sets the grid lights.
  std::array<double, 2> xy{};       ///< CIE 1931 chromaticity, with the tint
};

/**
 * Rule 'as_shot_tint' (Film Grain plan, phase G8): the colour of a scene light off the Planckian
 * locus. The grid lights of As Shot are on the Planckian locus at 4000 K and below and on the CIE
 * daylight locus at 5000 K and above. A RAW white off that locus (a fluorescent, mercury, sodium,
 * or LED light, or a green shade) moves the light across the locus by the factor
 * 10^(k g(lambda)), with g the tint band below, and the Wien factor of a mired change keeps the
 * light at the colour temperature of the grid light. The light keeps its luminance Y.
 */
namespace film_scene_tint {

/// Centre and standard deviation of the tint band g, in nm. Inferred: the green lines of mercury
/// (546 nm) and of the terbium phosphor of fluorescent and LED lights (545 nm) are near the
/// centre, and the width keeps the factor smooth.
inline constexpr double kTintBandCentreNm = 550.0;
inline constexpr double kTintBandSigmaNm  = 50.0;

/**
 * Limit of |k| of the tint factor: a factor of 10 at the centre of the band. Measured on the lab
 * grid lights: the corners of the Develop range (2000 to 15000 K, Duv -0.05 to +0.05) with a
 * chromaticity in the spectrum locus need |k| <= 0.78. Near the spectrum locus (at 2000 K the
 * Planckian locus is about 0.01 from it in CIE 1960 uv), a RAW white needs a light near a single
 * wavelength, and k grows with no end. At the limit the light keeps the colour temperature of the
 * grid light and moves toward the RAW white as far as the limit lets it.
 */
inline constexpr double kTintGainLimit    = 1.0;

/// g(lambda) of the tint factor: a Gaussian with a peak of 1 at @ref kTintBandCentreNm.
[[nodiscard]] auto      TintBand(double wavelength_nm) -> double;

/// Second radiation constant c2 of the Planck law, nm K (CODATA 2018: 1.438777e-2 m K).
inline constexpr double kSecondRadiationConstantNmK = 1.438777e7;

/**
 * @brief The natural log of the Wien factor of a change of 1 mired: -c2 1e-6 / lambda.
 *
 * In the Wien approximation of the Planck law, a light of temperature T has the spectrum
 * lambda^-5 exp(-c2 / (lambda T)), so the factor exp(dm * this value) moves it by dm mired. The
 * tint factor also moves the colour temperature of a light; the Wien factor takes that change back
 * with no limit of the grid of lights.
 */
[[nodiscard]] auto      WienLogPerMired(double wavelength_nm) -> double;

/**
 * @brief CIE 1931 XYZ of a spectrum: the sum over the nodes of the spectrum times the colour
 * matching functions (CIE 018:2019, 1 nm), linear between their 1 nm nodes and 0 outside 360 to
 * 830 nm. The scale is that of a sum over the nodes, so only ratios have a meaning.
 */
[[nodiscard]] auto      SpectrumXyz(std::span<const double> wavelengths_nm,
                                    std::span<const double> spectrum) -> std::array<double, 3>;

/// CIE 1960 uv of XYZ.
[[nodiscard]] auto      Uv1960(const std::array<double, 3>& xyz) -> std::array<double, 2>;
/// CIE 1960 uv of CIE 1931 xy.
[[nodiscard]] auto      Uv1960OfXy(const std::array<double, 2>& xy) -> std::array<double, 2>;

/// The place of a CIE 1960 uv relative to the Planckian locus.
struct LocusOffset {
  double                duv = 0.0;  ///< Signed distance to the locus; above 0 toward green
  std::array<double, 2> normal{};   ///< Unit normal of the locus at the nearest point, toward green
};

/**
 * @brief The Duv of @p uv: the signed distance to the nearest point of the Planckian locus table
 * of Develop (`planckian_locus::kUvTable`, 1000 to 20000 K), with the locus linear between its
 * nodes. Positive is above the locus (green), as a positive tint of Develop.
 */
[[nodiscard]] auto PlanckianOffset(const std::array<double, 2>& uv) -> LocusOffset;

}  // namespace film_scene_tint
}  // namespace alcedo
