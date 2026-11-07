//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

#include "edit/graph/film_print_model.hpp"
#include "edit/runtime/content_key.hpp"
#include "edit/runtime/film/film_stock_bundle.hpp"

/// Host values of print material Negative Scan (lab step P14, issue 661): the virtual scanner, the
/// values of a selection for a scanner, the terms of one render, and lab rule 'frame_auto_colour'
/// on a host prescan. The lab source is `scripts/poc/p14_negative_scan/negative_scan.py` of
/// alcedo-film-lab.
namespace alcedo::film_negative_scan {

/// The point form of the lab for the grey records of the stored values: INHIBITION_STEPS_POINT of
/// `film_preview.py`. The kernels use rule 'inhibition_two_steps' instead.
inline constexpr int kPointInhibitionSteps = 16;

/// The virtual scanner of lab rule 'ideal_scanner_bands'.
struct Scanner {
  std::array<double, 3> centres_nm{};  ///< R, G, B
  double                width_nm     = 0.0;
  bool                  equal_energy = false;  ///< False: the halogen lamp

  auto                  operator==(const Scanner&) const -> bool = default;
};

/// The default scanner of the resources.
[[nodiscard]] auto DefaultScanner(const FilmNegativeScanConstants& constants) -> Scanner;
/// The scanner of the document settings.
/// @throws std::runtime_error for an unknown scanner light.
[[nodiscard]] auto ScannerOf(const FilmNegativeScanParams& params) -> Scanner;

/**
 * @brief The weights L(lambda) S_i(lambda) of a scanner on the wavelengths of the bundle.
 *
 * Lab `ScannerSetup.responsivities`: a Gaussian band per channel with sigma = width / (2 sqrt(2
 * ln 2)), cut to 0 below @ref FilmNegativeScanConstants::band_cut of its peak, on the nodes from the
 * first to the last wavelength where a band is at that cut or above; times the lamp
 * lambda^-5 / (exp(c2 / (lambda T)) - 1), or 1 for an equal-energy light. Zero outside those nodes.
 */
struct ScannerWeights {
  std::int64_t        first_node = 0;  ///< Index of the first band node in the wavelength list
  std::int64_t        nodes      = 0;  ///< Count of band nodes
  std::vector<double> weights;         ///< (3, wavelengths)
};

/// @throws std::runtime_error when a centre or the width is outside the lab limits, or when no
///         wavelength is in a band.
[[nodiscard]] auto MakeScannerWeights(const Scanner& scanner,
                                      const FilmNegativeScanConstants& constants,
                                      std::span<const double>          wavelengths_nm)
    -> ScannerWeights;

/// D_base: log10(sum w_i) - log10(sum w_i 10^-base), the scan density of the unexposed film.
[[nodiscard]] auto BaseDensity(const ScannerWeights& weights, std::span<const double> base)
    -> std::array<double, 3>;

/// A scanner table of one negative: T on the record nodes of the print table, (edge^3, 3), with red
/// varying slowest and blue fastest.
struct ScannerTableView {
  std::span<const double> values;
  std::int64_t            edge = 0;
};

/**
 * @brief The trilinear read of a scanner table at a record, as numpy `map_coordinates` with order
 * 1 and mode 'nearest' on the record grid of @p negative (lab `ScanPreview.table_apd`).
 */
[[nodiscard]] auto ReadScannerTable(const ScannerTableView& table, const FilmNegativeView& negative,
                                    const std::array<double, 3>& record) -> std::array<double, 3>;

/**
 * @brief The record (dye amounts) of a uniform layer exposure @p q in the lab point form:
 * `FilmPreview.negative_density` with @ref kPointInhibitionSteps steps of rule
 * 'development_inhibition'.
 */
[[nodiscard]] auto PointRecord(const FilmNegativeView& negative, const std::array<double, 3>& q)
    -> std::array<double, 3>;

/// The values of a selection for a scanner, before the rendering settings.
struct SelectionValues {
  std::array<double, 3> base_density{};  ///< D_base
  std::array<double, 3> aim_density{};   ///< D_aim, lab rule 'stock_balance'
  /// Lab rule 'frame_auto_colour', contrast 1: the change of x of an 18 % grey from T3200 to D65,
  /// with no term of rule 'medium_light_cast'. The auto colour reads @ref ReadLightAxis.
  std::array<double, 3> light_axis{};
  std::array<double, 3> gamma{};  ///< The gamma of the medium
  /// Rule 'medium_light_cast' (issue 705): the term of the scene light that the scanner software
  /// adds to (D - D_aim) / gamma, in log10 exposure units at contrast 1, with a sum of 0. It is 0
  /// until @ref WithMediumLightCast sets it.
  std::array<double, 3> light_cast{};
  /// Rule 'medium_light_cast': the term of D65 less the term of T3200, the change that the rule
  /// gives to the light axis.
  std::array<double, 3> light_axis_cast{};

  auto                  operator==(const SelectionValues&) const -> bool = default;
};

/// The scanner table of a negative for the scanner of a derivation.
using ScannerTableOf = std::function<ScannerTableView(std::string_view negative)>;

/**
 * @brief D - D_base of a neutral of @p value under the light @p spectrum, with the layer gain
 * @p gain: lab `VirtualScan.grey_density` with a stored gain and spectrum.
 */
[[nodiscard]] auto GreyDensity(const FilmStockBundle& bundle, std::string_view negative,
                               const ScannerTableView&      table,
                               const std::array<double, 3>& base_density,
                               const std::array<double, 3>& gain, std::span<const double> spectrum,
                               double value) -> std::array<double, 3>;

/**
 * @brief Lab rule 'generic_density_inversion' (`derive_medium`): the gamma of @p medium for the
 * scanner of @p weights and @p table_of, the mean over the pool of the channel density per log10
 * exposure of the 18 % grey (central difference at 0.09 and 0.36), each stock under its balance
 * light with the layer gain of its pool scene light.
 * @throws std::runtime_error that names the medium when a gamma is not above 0.
 */
[[nodiscard]] auto DeriveMediumGamma(const FilmStockBundle& bundle, const FilmScanMedium& medium,
                                     const ScannerWeights& weights, const ScannerTableOf& table_of)
    -> std::array<double, 3>;

/**
 * @brief The values of a selection for a scanner (lab `NegativeScanPreview.__init__`): D_base,
 * D_aim of the balance light, and the light axis from T3200 to D65, with the layer gain of
 * @p setup and @p gamma.
 */
[[nodiscard]] auto DeriveSelectionValues(const FilmStockBundle& bundle, std::string_view negative,
                                         const FilmNegativeScanSetup& setup,
                                         const ScannerWeights&        weights,
                                         const ScannerTableView&      table,
                                         const std::array<double, 3>& gamma) -> SelectionValues;

/// The stored values of a selection for the default scanner.
[[nodiscard]] auto StoredSelectionValues(const FilmNegativeScanSetup& setup) -> SelectionValues;

/**
 * @brief D - D_base of the uniform layer exposure @p q, from the spectral integral of the scanner
 * (lab `ScanPreview.apd`), with no scanner table: log10(sum w) - log10(sum w 10^-(base + dyes a))
 * of the record a = @ref PointRecord.
 */
[[nodiscard]] auto ScanDensityOfExposure(const FilmNegativeView&      negative,
                                         const ScannerWeights&        weights,
                                         const std::array<double, 3>& q) -> std::array<double, 3>;

/**
 * @brief Rule 'medium_light_cast' (issue 705): the colorimetric cast of @p light seen with the
 * white of @p balance, in log10 AP1 with a sum of 0.
 *
 * XYZ of each light is the CIE 1931 integral of `film_scene_tint::SpectrumXyz`. The CIE CAT02
 * adaptation (von Kries in CAT02 LMS, as Color Grade white balance) takes @p balance to the AP1
 * white, and the same adaptation acts on @p light. The cast is log10 of the AP1 of the adapted
 * @p light less log10 of the AP1 of the adapted @p balance (the AP1 white up to rounding), less
 * the mean of the three values. So @p balance itself has a cast of exactly 0.
 *
 * @throws std::runtime_error when an AP1 value of the adapted light is not above 0.
 */
[[nodiscard]] auto ColorimetricLightCast(const FilmStockBundle&  bundle,
                                         std::span<const double> light,
                                         std::span<const double> balance) -> std::array<double, 3>;

/**
 * @brief Rule 'medium_light_cast': the cast of @p light against @p balance on the average negative
 * of @p medium, as the scan reads it.
 *
 * For each stock of the pool (the pool of rule 'generic_density_inversion', with the layer gain of
 * its pool scene light), the change of (D - D_base) / gamma of an 18 % grey from @p balance to
 * @p light, from @ref ScanDensityOfExposure; the mean over the pool, less the mean of the three
 * values.
 */
[[nodiscard]] auto MediumFilmCast(const FilmStockBundle& bundle, const FilmScanMedium& medium,
                                  const ScannerWeights& weights, const std::array<double, 3>& gamma,
                                  std::span<const double> light, std::span<const double> balance)
    -> std::array<double, 3>;

/**
 * @brief Rule 'medium_light_cast' (issue 705): the scanner software of a medium reads the cast of
 * a scene light on the average negative of the medium as the colorimetric cast of that light.
 *
 * The layer sensitivities of a negative are narrower and further apart than the AP1 colour
 * matching functions, so the scan channels read as AP1 give the cast of a warm light about 10 deg
 * redder and up to 30 % more saturated than the light (lab probe of issue 705). This term moves the
 * x of a grey of each stock by @ref ColorimetricLightCast - @ref MediumFilmCast. So the grey of the
 * average negative has the colorimetric cast, and each stock keeps its own difference from the
 * average negative (its sensitivities, dyes, curves, and inhibition). The term has a sum of 0, so
 * the level of the grey stays. As the gamma of the medium, it is the same for all stocks of a
 * medium with one balance light: it depends on the medium, the scanner, the balance light, and the
 * scene light. It is 0 under the balance light, so the film renders the colours of that light as
 * before.
 *
 * @return @p values with @ref SelectionValues::light_cast of @p scene_spectrum and
 *         @ref SelectionValues::light_axis_cast of the axis lights.
 */
[[nodiscard]] auto WithMediumLightCast(const SelectionValues& values, const FilmStockBundle& bundle,
                                       std::string_view negative, const ScannerWeights& weights,
                                       std::span<const double> scene_spectrum) -> SelectionValues;

/// @p values with the terms of rule 'medium_light_cast' at 0: the reading of the lab, which takes
/// the scan channels as AP1. Tests compare this reading with the lab values.
[[nodiscard]] auto WithoutMediumLightCast(const SelectionValues& values) -> SelectionValues;

/// The light axis of rule 'frame_auto_colour' for the reading of @p values: the lab light axis plus
/// @ref SelectionValues::light_axis_cast.
[[nodiscard]] auto ReadLightAxis(const SelectionValues& values) -> std::array<double, 3>;

/// The shift of x before the auto colour: exposure log10(2) - (cyan, magenta, yellow).
[[nodiscard]] auto ManualShift(const FilmNegativeScanParams& params) -> std::array<double, 3>;

/// The terms of one render, in the form of `FilmKernelNegativeScan` of film_print_math.h.
struct RenderTerms {
  std::array<double, 3> slope{};
  std::array<double, 3> offset{};
  std::array<double, 3> scale{};
  std::array<double, 3> floor{};
};

/**
 * @brief `FilmKernelNegativeScanTerms` of film_print_math.h in double precision, with the shift
 * @p auto_shift + @ref ManualShift, and with the aim D_aim - gamma light_cast, so that
 * x = contrast ((D - D_aim) / gamma + light_cast) + shift (rule 'medium_light_cast').
 * @throws std::runtime_error that names the black when black E_base is at or above 0.18 in a
 *         channel: the lab stops there ("lower the black share").
 */
[[nodiscard]] auto MakeRenderTerms(const SelectionValues&        values,
                                   const FilmNegativeScanParams& params,
                                   const std::array<double, 3>&  auto_shift) -> RenderTerms;

/// The terms with a shift of 0 and the contrast of @p params: the x of the prescan of rule
/// 'frame_auto_colour' is slope T + offset, with the light cast of @ref MakeRenderTerms.
[[nodiscard]] auto MakePrescanTerms(const SelectionValues&        values,
                                    const FilmNegativeScanParams& params) -> RenderTerms;

/// The constants of lab rule 'frame_auto_colour' (`negative_scan.py`; inferred values, stated in
/// section 3.3 of the P14 report).
struct AutoColourConstants {
  int                   long_edge       = 96;
  double                valid_low       = -1.6;  ///< log10 about the grey
  double                valid_high      = 0.9;
  double                sigma           = 0.12;
  int                   iterations      = 4;
  double                cross_share     = 0.5;
  double                highlight_share = 0.05;
  std::array<double, 4> target{0.0, 0.0, 0.04, -0.04};  ///< off, neutral, warm, cool
  int                   minimum_pixels  = 16;
};
inline constexpr AutoColourConstants kAutoColour{};

/// A frame of 3 values per pixel, rows of columns.
struct Frame {
  std::vector<double> values;  ///< (rows, columns, 3)
  std::int64_t        rows    = 0;
  std::int64_t        columns = 0;
};

/// The shape of the prescan of a frame: the long edge reduced to @ref AutoColourConstants::long_edge
/// with numpy-style round half to even, and at least 1; a frame no larger keeps its shape.
[[nodiscard]] auto PrescanShape(std::int64_t rows, std::int64_t columns)
    -> std::array<std::int64_t, 2>;

/**
 * @brief The prescan of rule 'frame_auto_colour': lab `reduce_frame`, the area reduction of Pillow
 * `Image.resize` with filter BOX on each channel as a float32 image (the horizontal pass, then the
 * vertical pass, each stored as float32).
 */
[[nodiscard]] auto ReduceFrame(const Frame& frame) -> Frame;

/**
 * @brief The windows of one axis of @ref ReduceFrame, for the GPU form of the prescan.
 *
 * Per output pixel, the first input pixel and the count of input pixels with a weight above 0 in
 * Pillow `precompute_coeffs` with the BOX filter. Those pixels are adjacent, and each has the weight
 * 1 / count, so an output pixel is the mean of its window. An axis that keeps its size has the
 * window of one pixel each, which gives the value with no change, as Pillow skips that pass.
 */
struct PrescanAxis {
  std::vector<std::int32_t> first;
  std::vector<std::int32_t> count;
};

/// @throws std::invalid_argument when @p input or @p output is not above 0.
[[nodiscard]] auto PrescanAxisOf(std::int64_t input, std::int64_t output) -> PrescanAxis;

/**
 * @brief The GPU form of the prescan for one render (issue 664).
 *
 * The prescan has the shape @ref PrescanShape of the frame (the edit frame), so a full-edit render
 * and a viewport ROI render name the same canonical result. A render that builds it reduces its own
 * pixels with the windows of @ref PrescanAxisOf: a horizontal pass and a vertical pass, each stored
 * as float32. Each output pixel of a pass is the mean of the input pixels of its window.
 *
 * @ref windows has 4 parts of int32: the first input column of each output column, the count of
 * those columns, then the first input row of each output row, and the count of those rows.
 */
struct PrescanPlan {
  std::int64_t              rows    = 0;
  std::int64_t              columns = 0;
  std::vector<std::int32_t> windows;
  ContentKey                windows_key{};  ///< Content key of @ref windows, for a keyed buffer
};

/// @throws std::invalid_argument when the frame or the render is empty.
[[nodiscard]] auto PrescanPlanOf(std::int64_t frame_rows, std::int64_t frame_columns,
                                 std::int64_t render_rows, std::int64_t render_columns)
    -> PrescanPlan;

/// Lab `auto_colour_shift` on a prescan: the near-neutral and the white-patch estimates of the
/// cast, rule 'smaller_cast', and the correction along and across the light axis.
struct AutoColourResult {
  std::array<double, 3> shift{};  ///< Add to x
  std::array<double, 3> cast{};
  std::array<double, 3> near_neutral_cast{};
  std::array<double, 3> white_patch_cast{};
  std::int64_t          prescan_pixels = 0;  ///< Valid prescan pixels
};

/**
 * @param mode One of @ref kFilmScanAutoIds. `off` gives a shift of 0 and reads no pixel.
 * @throws std::runtime_error when fewer than 16 prescan pixels are inside the valid range, as the
 *         lab.
 */
[[nodiscard]] auto AutoColourShift(const Frame& prescan, const std::array<double, 3>& light_axis,
                                   std::string_view mode, double strength, double density_strength)
    -> AutoColourResult;

/**
 * @brief Rule 'frame_auto_colour' on a GPU prescan of the scan density (issue 664).
 *
 * The GPU reduces the scan density T, not x: x of the prescan is the affine map slope T + offset of
 * @ref MakePrescanTerms, and the area mean of an affine map is the map of the mean. So the prescan
 * does not depend on the contrast, and a rendering setting reads the kept prescan. The map runs
 * here in double precision; then @ref AutoColourShift with the auto settings of @p params and the
 * light axis @ref ReadLightAxis.
 *
 * @param density_rgba The prescan of T: rows x columns texels of 4 floats, the fourth unused.
 * @throws std::runtime_error as @ref AutoColourShift, and when the size of @p density_rgba is not
 *         rows x columns x 4.
 */
[[nodiscard]] auto AutoColourOfDensityPrescan(std::span<const float> density_rgba,
                                              std::int64_t rows, std::int64_t columns,
                                              const SelectionValues&        values,
                                              const FilmNegativeScanParams& params)
    -> AutoColourResult;

}  // namespace alcedo::film_negative_scan
