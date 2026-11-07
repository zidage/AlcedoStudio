//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../graph/test_camera_profile.hpp"
#include "edit/graph/develop_color_transform.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/film_print_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/runtime/film/film_print_resources.hpp"
#include "edit/runtime/film/film_print_tables.hpp"
#include "edit/runtime/film/film_scene_tint.hpp"

/// Helpers of the GPU film print tests, shared by the CUDA and the OpenCL fixtures: the white
/// balance of the RAW decode that scene light As Shot reads (issue 661), and the comparison of
/// print material Negative Scan with the lab before the display transform, in stops (issue 664).
namespace alcedo::film_print_test {

/// Develop mode As Shot, with @p kelvin and @p tint as the solved white of the RAW file. The test
/// camera profile makes the colour transform of Develop resolve: with no resolved transform the
/// document has no RAW white balance (issue 672).
inline void SetRawAsShot(PipelineDocument& document, double kelvin, double tint = 0.0) {
  gpu_dag_test::EnsureTestCameraProfile(document);
  auto payload         = document.Develop()->Params().Params();
  payload.wb_mode      = "as_shot";
  payload.as_shot_cct  = static_cast<float>(kelvin);
  payload.as_shot_tint = static_cast<float>(tint);
  payload.input.reset();
  document.Develop()->Params().ReplaceParams(std::move(payload));
}

/// Develop mode Custom with @p kelvin and @p tint, with the test camera profile (see
/// @ref SetRawAsShot).
inline void SetRawCustom(PipelineDocument& document, double kelvin, double tint = 0.0) {
  gpu_dag_test::EnsureTestCameraProfile(document);
  DevelopColorTemperatureUpdate update;
  update.wb_mode     = std::string{"custom"};
  update.custom_cct  = static_cast<float>(kelvin);
  update.custom_tint = static_cast<float>(tint);
  document.Develop()->Params().ApplyColorTemperatureUpdate(update);
}

/**
 * @brief The Develop tint of a RAW white with the Duv of the grid light of @p kelvin.
 *
 * The lab files of a `cct<kelvin>` label hold the grid light, and the lab has no tint. With this
 * tint, rule 'as_shot_tint' moves the light by less than 1e-5 Duv, so As Shot is that grid light.
 */
inline auto GridLightTint(std::string_view negative, double kelvin) -> double {
  const auto& bundle = SharedFilmPrintTables().Bundle();
  const auto  spectrum =
      bundle.NegativeScanSelection(negative, bundle.GridLight(kelvin)).scene_spectrum;
  const auto   xyz   = film_scene_tint::SpectrumXyz(bundle.WavelengthsNm(), spectrum);
  const double sum   = xyz[0] + xyz[1] + xyz[2];
  const auto   white = WhiteBalanceXyToTemperatureTint({xyz[0] / sum, xyz[1] / sum});
  if (!white.has_value()) {
    throw std::runtime_error("The grid light has no Develop temperature and tint");
  }
  return white->tint;
}

/// A raster input (JPEG, PNG, TIFF), which has no RAW white balance.
inline void SetRasterInput(PipelineDocument& document) {
  auto payload  = document.Develop()->Params().Params();
  payload.input = DevelopRasterInput{};
  document.Develop()->Params().ReplaceParams(std::move(payload));
}

/// The colour temperature of a light label of the lab files: `cct3100` is 3100 K.
inline auto KelvinOfLabel(const std::string& label) -> std::optional<double> {
  if (label.rfind("cct", 0) != 0) return std::nullopt;
  return std::stod(label.substr(3));
}

/// The auto colour strength of the lab files: the default of the lab viewer
/// (`mvp_pipeline.RenderSettings.neg_scan_auto_strength`). The files hold no strength.
inline constexpr float kLabAutoStrength = 0.8f;

/**
 * @brief Print material Negative Scan on @p document with auto colour @p auto_colour at the
 * strength of the lab files, under the light of the lab label @p scene_light: a named light, or As
 * Shot with the solved temperature of the RAW decode at the Kelvin of a `cct<kelvin>` label (a
 * blend of the grid).
 */
inline void SetNegativeScan(PipelineDocument& document, const std::string& scene_light,
                            const std::string& auto_colour) {
  const auto         kelvin = KelvinOfLabel(scene_light);
  DrtParameterUpdate update;
  update.film_print_material     = std::string{kFilmNegativeScanId};
  update.film_scan_auto          = auto_colour;
  update.film_scan_auto_strength = kLabAutoStrength;
  update.film_scene_light = kelvin.has_value() ? std::string{kFilmAsShotLightId} : scene_light;
  document.Drt()->Params().ApplyUpdate(update);
  if (kelvin.has_value()) {
    const auto& negative = document.Drt()->Params().FilmPrint().negative;
    SetRawAsShot(document, *kelvin, GridLightTint(negative, *kelvin));
  }
}

/// The scan tests compare values above 2^-10 in stops: below it the black of the form has its
/// floor.
inline constexpr double kScanFloor = 1.0 / 1024.0;

/// The largest |log2(a / b)| of the channels where both values are above @ref kScanFloor, or 0.
inline auto StopsDifference(const std::array<double, 3>& a, const std::array<double, 3>& b)
    -> double {
  double largest = 0.0;
  for (std::size_t k = 0; k < 3; ++k) {
    if (a[k] > kScanFloor && b[k] > kScanFloor) {
      largest = (std::max)(largest, std::abs(std::log2(a[k] / b[k])));
    }
  }
  return largest;
}

/// A channel under this share of the brightest channel of its lab value is not compared in stops
/// by @ref ChartStopsDifference.
inline constexpr double kChartRelativeFloor = 1.0 / 256.0;

/**
 * @brief @ref StopsDifference for a chart value, with the floor also at @ref kChartRelativeFloor of
 * the brightest channel of the lab value @p b.
 *
 * Far out of AP1, FP1 reads the u'v' table at the edge of the hull, where a layer can fall from its
 * value to 0 within one cell; a float32 read there differs from the double form by up to 2 % in that
 * layer (issue 667). The scan shows such a layer near its black, so a channel far below the others
 * of its pixel has a large difference in stops and a small one in light.
 */
inline auto ChartStopsDifference(const std::array<double, 3>& a, const std::array<double, 3>& b)
    -> double {
  const double floor = (std::max)(kScanFloor, kChartRelativeFloor * (std::max)({b[0], b[1], b[2]}));
  double       largest = 0.0;
  for (std::size_t k = 0; k < 3; ++k) {
    if (a[k] > floor && b[k] > floor) {
      largest = (std::max)(largest, std::abs(std::log2(a[k] / b[k])));
    }
  }
  return largest;
}

template <class Pixel>
auto Rgb(const Pixel& pixel) -> std::array<double, 3> {
  return {pixel.r, pixel.g, pixel.b};
}

/// The largest difference in stops between linear AP1 pixels and linear AP1 values of the lab.
template <class Pixel>
auto LargestStops(const std::vector<Pixel>& pixels, std::size_t count, std::span<const float> lab)
    -> double {
  double largest = 0.0;
  for (std::size_t index = 0; index < count; ++index) {
    largest = (std::max)(largest, StopsDifference(Rgb(pixels[index]),
                                                  {lab[index * 3], lab[index * 3 + 1],
                                                   lab[index * 3 + 2]}));
  }
  return largest;
}

/// Per pixel the largest channel difference in stops: the mean, p99.9, and the largest.
struct StopsStatistics {
  double mean    = 0.0;
  double p999    = 0.0;
  double largest = 0.0;
};

template <class Pixel>
auto ImageStops(const std::vector<Pixel>& pixels, std::span<const float> lab) -> StopsStatistics {
  std::vector<float> difference(pixels.size());
  double             sum = 0.0;
  for (std::size_t index = 0; index < pixels.size(); ++index) {
    difference[index] = static_cast<float>(StopsDifference(
        Rgb(pixels[index]), {lab[index * 3], lab[index * 3 + 1], lab[index * 3 + 2]}));
    sum += difference[index];
  }
  StopsStatistics result;
  result.mean     = sum / static_cast<double>(difference.size());
  const auto rank = static_cast<std::ptrdiff_t>(0.999 * static_cast<double>(difference.size() - 1));
  std::nth_element(difference.begin(), difference.begin() + rank, difference.end());
  result.p999    = difference[static_cast<std::size_t>(rank)];
  result.largest = *std::max_element(difference.begin() + rank, difference.end());
  return result;
}

/// The largest channel difference of two shifts of x, in log10 units.
inline auto LargestShiftDifference(const std::array<double, 3>& a, std::span<const double> b)
    -> double {
  return (std::max)({std::abs(a[0] - b[0]), std::abs(a[1] - b[1]), std::abs(a[2] - b[2])});
}

}  // namespace alcedo::film_print_test
