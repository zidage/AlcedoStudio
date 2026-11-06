//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "edit/geometry/resolved_render_geometry.hpp"
#include "edit/graph/film_format.hpp"
#include "edit/runtime/film/film_grain_field.h"
#include "edit/runtime/film/film_stock_bundle.hpp"

namespace alcedo {

/// Largest blur radius of rule 'grain_kernel', in taps on each side of the centre.
inline constexpr std::uint32_t kFilmGrainMaxBlurRadius = 24;
/// Largest k of rule 'grain_aggregate': reference pixels summed per render pixel and axis.
inline constexpr std::uint32_t kFilmGrainMaxAggregate  = 4;

/// Where the grain goes. It selects the curves and the correlation of the plan.
enum class FilmGrainTarget : std::uint8_t {
  /// The DRT/Post Film Grain pass of ACES 2.0 and OpenDRT: the channel curves of the stock, and the
  /// display correlation of `common/grain_post_display_correlation`.
  Post     = 0,
  /// The dye amounts of the negative of the film print: the layer curves of the stock, and the
  /// layer correlation of `common/grain_layer_correlation`.
  Negative = 1,
};

/**
 * @brief The blur and the amplitude of one layer for one pixel pitch (rule 'grain_kernel').
 *
 * The blur is a discrete Gaussian with `sigma_b` in render pixels. `sigma_b` gives the field the
 * lag-1 correlation of the continuous pixel field. The separable blur of unit white noise has the
 * standard deviation @ref weight_square_sum, so the unit field is the blur divided by it, and the
 * pixel standard deviation of the layer is `sigma_48 * pixel_factor` exactly.
 */
struct FilmGrainLayerKernel {
  double                                          cloud_sigma_um = 0.0;  ///< sigma_c
  double                                          sigma_b_px     = 0.0;
  std::uint32_t                                   radius = 0;  ///< ceil(3 sigma_b), at most 24
  /// One half of the symmetric kernel: `weights[0]` is the centre. The full kernel sums to 1.
  /// Taps past @ref radius hold 0.
  std::array<double, kFilmGrainMaxBlurRadius + 1> weights{};
  double weight_square_sum = 0.0;  ///< Sum of w^2 over the full kernel of one axis
  double pixel_factor      = 0.0;  ///< sqrt(A_48) * I(sigma_c, p): sigma_px = sigma_48 * this
  double amplitude         = 0.0;  ///< pixel_factor / weight_square_sum
  double lag1              = 0.0;  ///< Lag-1 correlation of the blurred field, one axis
};

/**
 * @brief The host plan of the film grain of one render. A value type: the compile step copies it
 * into the kernel parameters.
 *
 * Rules of the plan 'film_grain_format_and_stock_plan.md' (sections 3.2 and 6.2) and of the lab
 * evaluator `grain_reference.py`: 'grain_pitch', 'grain_pixel_integral', 'grain_kernel',
 * 'grain_layer_mix', and 'grain_aggregate'.
 *
 * The kernel of a layer `k` adds `strength * sigma_k(v) * layers[k].amplitude * N_k`, with
 * `sigma_k` the curve @ref curves of the layer at the value `v` of the pixel, and `N = mix * u`,
 * `u` the three blurred unit fields.
 */
struct FilmGrainPlan {
  bool            enabled            = false;  ///< False when the strength is 0
  FilmGrainTarget target             = FilmGrainTarget::Post;
  FilmFormat      format             = FilmFormat::Film35mm;
  double          strength           = 0.0;  ///< The slider value: 1 is the datasheet granularity

  /// Rule 'grain_pitch': the frame long side / the long side of the full reference image.
  double          reference_pitch_um = 0.0;
  /// Reference pixels per render pixel, the mean of the two axes; at most 1.
  double          render_scale       = 1.0;
  double          render_pitch_um    = 0.0;  ///< reference_pitch_um / render_scale
  /// Rule 'grain_aggregate': clamp(round(1 / render_scale), 1, 4), 1 at a render scale of 1.
  std::uint32_t   aggregate          = 1;

  std::array<FilmGrainLayerKernel, 3> layers{};  ///< At the render pitch
  /// Rule 'grain_layer_mix': the lower Cholesky factor of the correlation, row major. Entries above
  /// the diagonal are 0.
  std::array<double, 9>               mix{};
  /// The layer curves (target Negative: dye amount to sigma of the amount) or the channel curves
  /// (target Post: gross density to sigma_48) of the stock.
  std::array<FilmGrainCurve, 3>       curves{};
};

/**
 * @brief Make the grain plan of one render. A pure function: it changes no state.
 *
 * @param geometry The render. Its full reference extent sets the pitch, and its render-to-reference
 *        mapping sets the render scale.
 * @param format The film format of the frame.
 * @param stock The grain data of the film: of the selected film of the film print, or of
 *        `kodak_5207` for a method with no film.
 * @param target The post pass or the negative of the film print.
 * @param strength The Film Grain slider, in [0, 2].
 * @throws std::invalid_argument that names the field for a format outside the enum, a strength
 *         outside [0, 2], a geometry with no reference extent, a stock curve with fewer than 2 or
 *         more than `FILM_GRAIN_CURVE_NODES` nodes, a cloud size that is not above 0, or a
 *         correlation that is not positive definite.
 */
[[nodiscard]] auto MakeFilmGrainPlan(const ResolvedRenderGeometry& geometry, FilmFormat format,
                                     const FilmGrainStockView& stock, FilmGrainTarget target,
                                     float strength) -> FilmGrainPlan;

/**
 * @brief Rule 'grain_pixel_integral': I(sigma_c, p) of one axis, in 1 / um.
 *
 * The integral over f of exp(-4 pi^2 sigma_c^2 f^2) sinc^2(p f), in its closed form
 * (1 / p) [erf(q / sqrt 2) - 2 (phi(0) - phi(q)) / q], q = p / (sqrt(2) sigma_c), phi the unit
 * normal density. For a large pixel it goes to 1 / p (Selwyn's law).
 */
[[nodiscard]] auto FilmGrainPixelIntegral(double cloud_sigma_um, double pitch_um) -> double;

/// The lag-1 correlation C(p) / C(0) of the continuous pixel field of one axis (rule
/// 'grain_kernel'), with C the covariance of a cloud blur read by a pixel box of side p.
[[nodiscard]] auto FilmGrainPixelLag1(double cloud_sigma_um, double pitch_um) -> double;

/// The kernel and the amplitude of one layer at the pitch @p pitch_um (rule 'grain_kernel').
[[nodiscard]] auto MakeFilmGrainLayerKernel(double cloud_sigma_um, double aperture_area_um2,
                                            double pitch_um) -> FilmGrainLayerKernel;

/**
 * @brief The segments of curve @p layer of @p curves (see `FilmGrainCurve`).
 * @throws std::invalid_argument that names @p field when the curve has fewer than 2 or more than
 *         `FILM_GRAIN_CURVE_NODES` nodes, or when its nodes do not rise.
 */
[[nodiscard]] auto MakeFilmGrainCurve(const FilmGrainCurveView& curves, std::size_t layer,
                                      std::string_view field) -> FilmGrainCurve;

}  // namespace alcedo
