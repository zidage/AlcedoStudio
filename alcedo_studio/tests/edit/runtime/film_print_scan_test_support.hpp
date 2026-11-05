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
#include <string>
#include <utility>
#include <vector>

#include "color/color_encoding_catalog.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/film_print_model.hpp"
#include "edit/graph/pipeline_document.hpp"

/// Helpers of the GPU tests of print material Lab Scan (issue 661), shared by the CUDA and the
/// OpenCL fixtures. The scan form is compared with the lab in ACES2065-1, before the display
/// transform, in stops.
namespace alcedo::film_print_scan_test {

/// The scan tests compare values above 2^-10 in stops: below it the transform has its floor.
inline constexpr double kSceneFloor = 1.0 / 1024.0;

/// AP1 to ACES2065-1 (AP0), from the primaries of the product, row major.
inline auto             Ap1ToAp0() -> const color::Matrix33d& {
  static const auto matrix =
      color::MultiplyMatrices(color::InvertMatrix(color::RgbToXyzMatrix(color::ColorGamutId::Ap0)),
                                          color::RgbToXyzMatrix(color::ColorGamutId::Ap1));
  return matrix;
}

/// ACES2065-1 of a pixel of linear AP1.
template <class Pixel>
auto Aces(const Pixel& pixel) -> std::array<double, 3> {
  const auto&  m = Ap1ToAp0();
  const double r = pixel.r;
  const double g = pixel.g;
  const double b = pixel.b;
  return {m[0] * r + m[1] * g + m[2] * b, m[3] * r + m[4] * g + m[5] * b,
          m[6] * r + m[7] * g + m[8] * b};
}

/// The largest |log2(a / b)| of the channels where both values are above @ref kSceneFloor, or 0.
inline auto StopsDifference(const std::array<double, 3>& a, const std::array<double, 3>& b)
    -> double {
  double largest = 0.0;
  for (std::size_t k = 0; k < 3; ++k) {
    if (a[k] > kSceneFloor && b[k] > kSceneFloor) {
      largest = (std::max)(largest, std::abs(std::log2(a[k] / b[k])));
    }
  }
  return largest;
}

/// The largest difference in stops between linear AP1 pixels and ACES2065-1 values of the lab.
template <class Pixel>
auto LargestStops(const std::vector<Pixel>& pixels, std::size_t count, std::span<const float> lab)
    -> double {
  double largest = 0.0;
  for (std::size_t index = 0; index < count; ++index) {
    largest = (std::max)(largest,
                         StopsDifference(Aces(pixels[index]),
                                         {lab[index * 3], lab[index * 3 + 1], lab[index * 3 + 2]}));
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
        Aces(pixels[index]), {lab[index * 3], lab[index * 3 + 1], lab[index * 3 + 2]}));
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

/// The colour temperature of a light label of the lab files: `cct3100` is 3100 K.
inline auto KelvinOfLabel(const std::string& label) -> std::optional<double> {
  if (label.rfind("cct", 0) != 0) return std::nullopt;
  return std::stod(label.substr(3));
}

/// Develop mode As Shot, with @p kelvin as the solved temperature of the RAW file.
inline void SetRawAsShot(PipelineDocument& document, double kelvin) {
  auto payload        = document.Develop()->Params().Params();
  payload.wb_mode     = "as_shot";
  payload.as_shot_cct = static_cast<float>(kelvin);
  payload.input.reset();
  document.Develop()->Params().ReplaceParams(std::move(payload));
}

/// Develop mode Custom with @p kelvin.
inline void SetRawCustom(PipelineDocument& document, double kelvin) {
  DevelopColorTemperatureUpdate update;
  update.wb_mode    = std::string{"custom"};
  update.custom_cct = static_cast<float>(kelvin);
  document.Develop()->Params().ApplyColorTemperatureUpdate(update);
}

/// A raster input (JPEG, PNG, TIFF), which has no RAW white balance.
inline void SetRasterInput(PipelineDocument& document) {
  auto payload  = document.Develop()->Params().Params();
  payload.input = DevelopRasterInput{};
  document.Develop()->Params().ReplaceParams(std::move(payload));
}

/// Print material Lab Scan on @p document, with the LAD aim, and with As Shot at @p kelvin when it
/// is set (a blend of the grid), else the named @p scene_light.
inline void SetLabScan(PipelineDocument& document, const std::string& scene_light, bool lad_aim) {
  DrtParameterUpdate update;
  update.film_print_material = std::string{kFilmLabScanId};
  update.film_lad_aim        = lad_aim;
  const auto kelvin          = KelvinOfLabel(scene_light);
  update.film_scene_light    = kelvin.has_value() ? std::string{kFilmAsShotLightId} : scene_light;
  document.Drt()->Params().ApplyUpdate(update);
  if (kelvin.has_value()) SetRawAsShot(document, *kelvin);
}

}  // namespace alcedo::film_print_scan_test
