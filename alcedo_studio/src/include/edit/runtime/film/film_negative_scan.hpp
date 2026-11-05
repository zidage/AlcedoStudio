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
  std::array<double, 3> light_axis{};    ///< Lab rule 'frame_auto_colour', contrast 1
  std::array<double, 3> gamma{};         ///< The gamma of the medium

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
 * @p auto_shift + @ref ManualShift.
 * @throws std::runtime_error that names the black when black E_base is at or above 0.18 in a
 *         channel: the lab stops there ("lower the black share").
 */
[[nodiscard]] auto MakeRenderTerms(const SelectionValues&        values,
                                   const FilmNegativeScanParams& params,
                                   const std::array<double, 3>&  auto_shift) -> RenderTerms;

/// The terms with a shift of 0 and the contrast of @p params: the x of the prescan of rule
/// 'frame_auto_colour' is slope T + offset.
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

}  // namespace alcedo::film_negative_scan
