//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

// The statistical checks of the grain in the negative of the film print on every backend (plan
// 'film_grain_format_and_stock_plan.md', phase G5 on CUDA, phase G6 on OpenCL and Metal). A backend
// test gives a render function that runs its grain kernel on an image of dye amounts; the checks
// are the same for each backend. The expected values come from the layer curves of the stock
// bundle and from the host form of the steps of the kernel. No stored pixels.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "edit/geometry/resolved_render_geometry.hpp"
#include "edit/graph/film_format.hpp"
#include "edit/runtime/film/film_grain_field.h"
#include "edit/runtime/film/film_grain_negative.h"
#include "edit/runtime/film/film_print_resources.hpp"
#include "edit/runtime/film_grain_plan.hpp"
#include "film_grain_post_checks.hpp"

namespace alcedo::film_grain_negative_test {

using film_grain_post_test::TileGeometry;

/// The record of the three layers of one pixel: three packed floats, as the images between the
/// steps of the film print.
struct Amounts {
  float r;
  float g;
  float b;
};

/// The print stocks of the lab evaluator (`print_stocks` of grain__expected.bin): two cine
/// negatives, a still negative of the tier `pgi`, and a colour reversal film.
inline constexpr std::array<const char*, 4> kPrintStocks{"kodak_5207", "kodak_5219",
                                                         "kodak_portra_400", "fuji_velvia_50"};

/// Runs the grain kernel of one backend on @p record, an image of the render size of @p geometry,
/// and returns the record with the grain.
using Render = std::function<std::vector<Amounts>(
    const FilmGrainPlan& plan, const ResolvedRenderGeometry& geometry,
    std::span<const double> reversal_dye_max, const std::vector<Amounts>& record)>;

/// The grain plan of the negative of @p film, from the shared film stock bundle.
inline auto Plan(const std::string& film, const ResolvedRenderGeometry& geometry, FilmFormat format,
                 float strength = 1.0f) -> FilmGrainPlan {
  return MakeFilmGrainPlan(geometry, format, SharedFilmPrintTables().Bundle().Grain(film),
                           FilmGrainTarget::Negative, strength);
}

/// The `dye_max` of @p film when it is a colour reversal film, else an empty span.
inline auto DyeMaxOf(const std::string& film) -> std::span<const double> {
  const auto& bundle = SharedFilmPrintTables().Bundle();
  return bundle.IsReversal(film) ? bundle.Reversal(film).dye_max : std::span<const double>{};
}

inline auto Layer(const Amounts& value, std::size_t layer) -> float {
  return layer == 0 ? value.r : (layer == 1 ? value.g : value.b);
}

/// The record of a pixel whose dye amounts are @p dye: the dye amounts of a negative, or
/// `dye_max - dye` of a reversal film.
inline auto RecordOf(const std::array<float, 3>& dye, std::span<const double> dye_max) -> Amounts {
  if (dye_max.empty()) return {dye[0], dye[1], dye[2]};
  return {static_cast<float>(dye_max[0]) - dye[0], static_cast<float>(dye_max[1]) - dye[1],
          static_cast<float>(dye_max[2]) - dye[2]};
}

/// The steps of the kernel on the host: the mixed grain of film_grain_post_checks.hpp, then the
/// apply step of film_grain_negative.h.
inline auto HostForm(const FilmGrainPlan& plan, const ResolvedRenderGeometry& geometry,
                     std::span<const double> reversal_dye_max, const std::vector<Amounts>& record)
    -> std::vector<Amounts> {
  const auto           params = MakeFilmGrainNegativeParams(plan, reversal_dye_max);
  const auto           grain  = film_grain_post_test::HostMixedGrain(plan, geometry);
  std::vector<Amounts> output(record.size());
  for (std::size_t index = 0; index < record.size(); ++index) {
    const auto& value = record[index];
    const auto result = FilmGrainNegativeRecord(&params, {value.r, value.g, value.b}, grain[index]);
    output[index]     = {result.r, result.g, result.b};
  }
  return output;
}

inline auto LargestDifference(const std::vector<Amounts>& a, const std::vector<Amounts>& b)
    -> float {
  float largest = 0.0f;
  for (std::size_t i = 0; i < a.size(); ++i) {
    largest = std::max(
        {largest, std::abs(a[i].r - b[i].r), std::abs(a[i].g - b[i].g), std::abs(a[i].b - b[i].b)});
  }
  return largest;
}

/**
 * The standard deviation of the dye amounts on a flat field equals sigma_a of the stock within 5 %,
 * at three amounts of each layer (a quarter, a half, and three quarters of the counted range of its
 * curve), for 5207, 5219, Portra 400, and Velvia 50.
 *
 * The dye amounts are box-averaged over boxes of B = 32 pixels at 35mm and 6000 px (6 um, so B is
 * 192 um), and Selwyn's law takes the box value to the 48 um aperture: sigma_48 = sigma_box * B /
 * sqrt(A_48). The 48 um aperture is near the size of the dye cloud, where the law does not hold
 * exactly, so the box is four times the aperture; there the pixel integral differs from 1 / B by
 * about 1.5 % (section 3.2). The 4096 boxes of a 2048 x 2048 render give a standard error of about
 * 1.1 %.
 */
inline void ExpectDyeAmountSigmaMatchesTheDatasheet(const Render& render) {
  constexpr std::uint32_t kSize    = 2048;
  constexpr std::uint32_t kBox     = 32;
  constexpr std::uint32_t kCells   = kSize / kBox;
  const auto              geometry = TileGeometry(6000, 1.0f, 900.0f, 700.0f, kSize, kSize);
  const auto&             common   = SharedFilmPrintTables().Bundle().GrainConstants();
  for (const std::string film : kPrintStocks) {
    const auto plan    = Plan(film, geometry, FilmFormat::Film35mm);
    const auto dye_max = DyeMaxOf(film);
    ASSERT_EQ(plan.reversal, !dye_max.empty()) << film;
    const double box_um = static_cast<double>(kBox) * plan.render_pitch_um;
    for (const double fraction : {0.25, 0.5, 0.75}) {
      std::array<float, 3> dye{};
      for (std::size_t layer = 0; layer < 3; ++layer) {
        dye[layer] = static_cast<float>(
            plan.curve_low[layer] + fraction * (plan.curve_high[layer] - plan.curve_low[layer]));
      }
      const std::vector<Amounts> record(static_cast<std::size_t>(kSize) * kSize,
                                        RecordOf(dye, dye_max));
      const auto                 output = render(plan, geometry, dye_max, record);
      for (std::size_t layer = 0; layer < 3; ++layer) {
        SCOPED_TRACE(film + " fraction " + std::to_string(fraction) + " layer " +
                     std::to_string(layer));
        std::vector<double> boxes;
        boxes.reserve(static_cast<std::size_t>(kCells) * kCells);
        for (std::uint32_t cy = 0; cy < kCells; ++cy) {
          for (std::uint32_t cx = 0; cx < kCells; ++cx) {
            double sum = 0.0;
            for (std::uint32_t y = cy * kBox; y < (cy + 1) * kBox; ++y) {
              for (std::uint32_t x = cx * kBox; x < (cx + 1) * kBox; ++x) {
                sum += Layer(output[static_cast<std::size_t>(y) * kSize + x], layer);
              }
            }
            boxes.push_back(sum / static_cast<double>(kBox * kBox));
          }
        }
        const double sigma_48 =
            film_grain_post_test::StdDev(boxes) * box_um / std::sqrt(common.aperture_area_um2);
        const double expected = FilmGrainCurveSigma(&plan.curves[layer], dye[layer]);
        EXPECT_NEAR(sigma_48 / expected, 1.0, 0.05);
      }
    }
  }
}

/// The per-pixel standard deviation of each layer on a flat field equals the plan,
/// `strength * sigma_a * pixel_factor`, within 5 % at 16mm, 35mm, and 70mm, for 5207 at 6000 px.
inline void ExpectPixelSigmaFollowsThePlanForEachFormat(const Render& render) {
  constexpr std::uint32_t kSize    = 512;
  const auto              geometry = TileGeometry(6000, 1.0f, 300.0f, 200.0f, kSize, kSize);
  for (const auto format : film_grain_post_test::kFormats) {
    const auto           plan = Plan("kodak_5207", geometry, format, 0.7f);
    std::array<float, 3> dye{};
    for (std::size_t layer = 0; layer < 3; ++layer) {
      dye[layer] = static_cast<float>(0.5 * (plan.curve_low[layer] + plan.curve_high[layer]));
    }
    const std::vector<Amounts> record(static_cast<std::size_t>(kSize) * kSize, RecordOf(dye, {}));
    const auto                 output = render(plan, geometry, {}, record);
    for (std::size_t layer = 0; layer < 3; ++layer) {
      SCOPED_TRACE(std::string(FilmFormatId(format)) + " layer " + std::to_string(layer));
      std::vector<double> values(output.size());
      for (std::size_t i = 0; i < output.size(); ++i) values[i] = Layer(output[i], layer);
      const double expected = 0.7 * plan.layers[layer].pixel_factor *
                              FilmGrainCurveSigma(&plan.curves[layer], dye[layer]);
      EXPECT_NEAR(film_grain_post_test::StdDev(values) / expected, 1.0, 0.05);
    }
  }
}

/// A 512 x 512 viewport region equals the same region of the full render within 1e-6. The region
/// origin is not a multiple of the 16 x 16 tile.
inline void ExpectViewportRegionEqualsTheSameRegionOfTheFullRender(const Render& render) {
  constexpr std::uint32_t kFull   = 1024;
  constexpr std::uint32_t kRegion = 512;
  constexpr std::uint32_t kLeft   = 250;
  constexpr std::uint32_t kTop    = 317;
  const auto              ramp    = [](const ResolvedRenderGeometry& geometry) {
    const auto           width  = geometry.render_extent.width;
    const auto           height = geometry.render_extent.height;
    const auto&          m      = geometry.render_to_reference.m;
    std::vector<Amounts> record(static_cast<std::size_t>(width) * height);
    for (std::uint32_t y = 0; y < height; ++y) {
      for (std::uint32_t x = 0; x < width; ++x) {
        const float rx = m[0] * (static_cast<float>(x) + 0.5f) + m[2];
        const float ry = m[4] * (static_cast<float>(y) + 0.5f) + m[5];
        record[static_cast<std::size_t>(y) * width + x] = {0.3f + 1.5f * rx / 1024.0f,
                                                           0.5f + 1.2f * ry / 1024.0f, 1.4f};
      }
    }
    return record;
  };
  const auto full_geometry   = TileGeometry(6000, 1.0f, 0.0f, 0.0f, kFull, kFull);
  const auto region_geometry = TileGeometry(6000, 1.0f, static_cast<float>(kLeft),
                                            static_cast<float>(kTop), kRegion, kRegion);
  const auto full_output     = render(Plan("kodak_5207", full_geometry, FilmFormat::Film16mm),
                                      full_geometry, {}, ramp(full_geometry));
  const auto region_output   = render(Plan("kodak_5207", region_geometry, FilmFormat::Film16mm),
                                      region_geometry, {}, ramp(region_geometry));
  float      largest         = 0.0f;
  for (std::uint32_t y = 0; y < kRegion; ++y) {
    for (std::uint32_t x = 0; x < kRegion; ++x) {
      const auto& a = region_output[static_cast<std::size_t>(y) * kRegion + x];
      const auto& b = full_output[static_cast<std::size_t>(y + kTop) * kFull + x + kLeft];
      largest = std::max({largest, std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b)});
    }
  }
  EXPECT_LE(largest, 1.0e-6f);
}

/// 16mm at 6000 px rendered at half size: a render of 37 x 29 with partial tiles and an aggregate
/// of 2, against the host form of the steps, for a negative and a reversal film.
inline void ExpectKernelEqualsTheHostFormOfItsStepsAtTheTileEdges(const Render& render) {
  constexpr std::uint32_t kWidth   = 37;
  constexpr std::uint32_t kHeight  = 29;
  const auto              geometry = TileGeometry(6000, 2.0f, 101.0f, 57.0f, kWidth, kHeight);
  for (const std::string film : {"kodak_5207", "fuji_velvia_50"}) {
    SCOPED_TRACE(film);
    const auto plan    = Plan(film, geometry, FilmFormat::Film16mm, 1.6f);
    const auto dye_max = DyeMaxOf(film);
    ASSERT_EQ(plan.aggregate, 2U);
    std::vector<Amounts> record(static_cast<std::size_t>(kWidth) * kHeight);
    for (std::uint32_t y = 0; y < kHeight; ++y) {
      for (std::uint32_t x = 0; x < kWidth; ++x) {
        const float t = static_cast<float>(x) / static_cast<float>(kWidth);
        const float u = static_cast<float>(y) / static_cast<float>(kHeight);
        record[static_cast<std::size_t>(y) * kWidth + x] =
            RecordOf({0.2f + 2.0f * t, 0.3f + 1.8f * u, 0.4f + t + u}, dye_max);
      }
    }
    EXPECT_LE(LargestDifference(render(plan, geometry, dye_max, record),
                                HostForm(plan, geometry, dye_max, record)),
              1.0e-5f);
  }
}

}  // namespace alcedo::film_grain_negative_test
