//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

// The statistical checks of the DRT/Post Film Grain of every backend (plan
// 'film_grain_format_and_stock_plan.md', phases G3 and G4). A backend test gives a render function
// that runs its grain pass on a display image; the checks are the same for CUDA, OpenCL, and Metal.
// The expected values come from the lab evaluator (grain__expected.bin of
// ALCEDO_FILM_PRINT_EXPECTED_DIR), from the host plan, and from the host form of the steps of the
// kernel. No stored pixels.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "color/color_encoding_math.h"
#include "edit/geometry/resolved_render_geometry.hpp"
#include "edit/graph/film_format.hpp"
#include "edit/runtime/drt_post_film_grain.hpp"
#include "edit/runtime/film/film_array_file.hpp"
#include "edit/runtime/film/film_grain_field.h"
#include "edit/runtime/film/film_grain_post.h"
#include "edit/runtime/film/film_print_resources.hpp"
#include "edit/runtime/film_grain_plan.hpp"

namespace alcedo::film_grain_post_test {

struct Rgba {
  float r;
  float g;
  float b;
  float a;
};

inline constexpr std::array<FilmFormat, 3>    kFormats{FilmFormat::Film16mm, FilmFormat::Film35mm,
                                                    FilmFormat::Film70mm};
inline constexpr std::array<std::uint32_t, 2> kLongSides{2560, 6000};
inline constexpr FilmGrainDisplayEncoding     kSrgb{CE_TF_SRGB, 1.0f};

/// Runs the grain pass of one backend on @p source, a display image of the render size of
/// @p geometry, and returns the output.
using Render = std::function<std::vector<Rgba>(
    const FilmGrainPlan& plan, const ResolvedRenderGeometry& geometry,
    const std::vector<Rgba>& source, FilmGrainDisplayEncoding encoding)>;

/**
 * A render of @p width x @p height in a 3:2 frame with the long side @p long_side. Render pixel
 * (0, 0) starts at reference pixel (@p origin_x, @p origin_y), and one render pixel covers
 * @p reference_per_render reference pixels on each axis.
 */
inline auto TileGeometry(std::uint32_t long_side, float reference_per_render, float origin_x,
                         float origin_y, std::uint32_t width, std::uint32_t height)
    -> ResolvedRenderGeometry {
  ResolvedRenderGeometry geometry;
  geometry.full_reference_extent    = {long_side, long_side * 2U / 3U};
  geometry.edit_extent              = geometry.full_reference_extent;
  geometry.render_extent            = {width, height};
  geometry.render_to_reference.m[0] = reference_per_render;
  geometry.render_to_reference.m[2] = origin_x;
  geometry.render_to_reference.m[4] = reference_per_render;
  geometry.render_to_reference.m[5] = origin_y;
  return geometry;
}

/// The grain plan of the post path with the 5207 data of the shared film stock bundle.
inline auto Plan(const ResolvedRenderGeometry& geometry, FilmFormat format, float strength = 1.0f)
    -> FilmGrainPlan {
  return MakeFilmGrainPlan(geometry, format,
                           SharedFilmPrintTables().Bundle().Grain(kFilmGrainPostFilm),
                           FilmGrainTarget::Post, strength);
}

/// The lab values, or null when ALCEDO_FILM_PRINT_EXPECTED_DIR has no grain__expected.bin.
inline auto LoadLabValues() -> std::unique_ptr<FilmArrayFile> {
#if defined(ALCEDO_FILM_PRINT_EXPECTED_DIR)
  const auto path = std::filesystem::path(ALCEDO_FILM_PRINT_EXPECTED_DIR) / "grain__expected.bin";
  if (std::filesystem::exists(path)) {
    return std::make_unique<FilmArrayFile>(FilmArrayFile::Parse(ReadFilmResourceBytes(path), path));
  }
#endif
  return nullptr;
}

inline auto Channel(const Rgba& pixel, int channel) -> float {
  return channel == 0 ? pixel.r : (channel == 1 ? pixel.g : pixel.b);
}

/// The density change -ln(y_out / y_in) of one channel, with y the linear light of sRGB: the
/// relative error of the linear light.
inline auto DensityChange(const std::vector<Rgba>& input, const std::vector<Rgba>& output,
                          int channel) -> std::vector<double> {
  std::vector<double> change(input.size());
  for (std::size_t i = 0; i < input.size(); ++i) {
    change[i] = -std::log(static_cast<double>(CeSrgbDecode(Channel(output[i], channel))) /
                          static_cast<double>(CeSrgbDecode(Channel(input[i], channel))));
  }
  return change;
}

inline auto Mean(const std::vector<double>& values) -> double {
  double sum = 0.0;
  for (const double value : values) sum += value;
  return sum / static_cast<double>(values.size());
}

inline auto StdDev(const std::vector<double>& values) -> double {
  const double mean = Mean(values);
  double       sum  = 0.0;
  for (const double value : values) sum += (value - mean) * (value - mean);
  return std::sqrt(sum / static_cast<double>(values.size()));
}

inline auto Correlation(const std::vector<double>& a, const std::vector<double>& b) -> double {
  const double ma = Mean(a);
  const double mb = Mean(b);
  double       ab = 0.0, aa = 0.0, bb = 0.0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    ab += (a[i] - ma) * (b[i] - mb);
    aa += (a[i] - ma) * (a[i] - ma);
    bb += (b[i] - mb) * (b[i] - mb);
  }
  return ab / std::sqrt(aa * bb);
}

/// The horizontal lag-1 correlation of an image of @p width columns.
inline auto HorizontalLag1(const std::vector<double>& values, std::uint32_t width) -> double {
  std::vector<double> left;
  std::vector<double> right;
  for (std::size_t i = 0; i + 1 < values.size(); ++i) {
    if ((i + 1) % width == 0) continue;
    left.push_back(values[i]);
    right.push_back(values[i + 1]);
  }
  return Correlation(left, right);
}

/// The 2 x 2 box mean of an image of @p width x @p height.
inline auto BoxHalf(const std::vector<double>& values, std::uint32_t width, std::uint32_t height)
    -> std::vector<double> {
  std::vector<double> out(static_cast<std::size_t>(width / 2) * (height / 2));
  for (std::uint32_t y = 0; y < height / 2; ++y) {
    for (std::uint32_t x = 0; x < width / 2; ++x) {
      const auto at = [&](std::uint32_t xx, std::uint32_t yy) {
        return values[static_cast<std::size_t>(yy) * width + xx];
      };
      out[static_cast<std::size_t>(y) * (width / 2) + x] =
          0.25 * (at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) + at(2 * x, 2 * y + 1) +
                  at(2 * x + 1, 2 * y + 1));
    }
  }
  return out;
}

/// A display image of code values from a function of the reference pixel of each render pixel.
inline auto MakeDisplay(const ResolvedRenderGeometry&            geometry,
                        const std::function<Rgba(float, float)>& at_reference)
    -> std::vector<Rgba> {
  const auto        width  = geometry.render_extent.width;
  const auto        height = geometry.render_extent.height;
  std::vector<Rgba> pixels(static_cast<std::size_t>(width) * height);
  const auto&       m = geometry.render_to_reference.m;
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const float rx = m[0] * (static_cast<float>(x) + 0.5f) + m[2];
      const float ry = m[4] * (static_cast<float>(y) + 0.5f) + m[5];
      pixels[static_cast<std::size_t>(y) * width + x] = at_reference(rx, ry);
    }
  }
  return pixels;
}

inline auto FlatSrgb(float linear) -> std::function<Rgba(float, float)> {
  const float code = CeSrgbEncode(linear);
  return [code](float, float) { return Rgba{code, code, code, 1.0f}; };
}

/// A flat grey of the post target of @p plan, as code values.
inline auto PostGrey(const FilmGrainPlan& plan, const ResolvedRenderGeometry& geometry)
    -> std::vector<Rgba> {
  return MakeDisplay(geometry, FlatSrgb(static_cast<float>(plan.post_grey)));
}

/**
 * The steps of the kernel on the host, in their mathematical order: noise, horizontal blur,
 * vertical blur, unit field, mix, apply. Every backend kernel equals this within the rounding of
 * its sums.
 */
inline auto HostForm(const FilmGrainPlan& plan, const ResolvedRenderGeometry& geometry,
                     const std::vector<Rgba>& input, FilmGrainDisplayEncoding encoding = kSrgb)
    -> std::vector<Rgba> {
  const auto field  = MakeFilmGrainFieldParams(plan, geometry);
  const auto post   = MakeFilmGrainPostParams(plan, encoding.transfer, encoding.decode_scale);
  const int  width  = static_cast<int>(geometry.render_extent.width);
  const int  height = static_cast<int>(geometry.render_extent.height);
  const int  halo   = field.halo;
  const int  span_x = width + 2 * halo;
  const int  span_y = height + 2 * halo;
  // The noise of the render and its halo, then the horizontal blur of each row.
  std::vector<std::array<float, 3>> noise(static_cast<std::size_t>(span_x) * span_y);
  for (int y = 0; y < span_y; ++y) {
    for (int x = 0; x < span_x; ++x) {
      const auto value = FilmGrainAggregateNoise(field.noise, x - halo, y - halo);
      noise[static_cast<std::size_t>(y) * span_x + x] = {value.x, value.y, value.z};
    }
  }
  std::vector<std::array<float, 3>> across(static_cast<std::size_t>(width) * span_y);
  for (int y = 0; y < span_y; ++y) {
    for (int x = 0; x < width; ++x) {
      for (int c = 0; c < 3; ++c) {
        const auto at = [&](int dx) {
          return noise[static_cast<std::size_t>(y) * span_x + x + halo + dx][c];
        };
        float sum = at(0) * field.weights[c][0];
        for (int tap = 1; tap <= halo; ++tap) {
          sum += (at(-tap) + at(tap)) * field.weights[c][tap];
        }
        across[static_cast<std::size_t>(y) * width + x][c] = sum;
      }
    }
  }
  std::vector<Rgba> output(input.size());
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      float unit[3];
      for (int c = 0; c < 3; ++c) {
        const auto at = [&](int dy) {
          return across[static_cast<std::size_t>(y + halo + dy) * width + x][c];
        };
        float sum = at(0) * field.weights[c][0];
        for (int tap = 1; tap <= halo; ++tap) {
          sum += (at(-tap) + at(tap)) * field.weights[c][tap];
        }
        unit[c] = sum * field.inverse_weight_square_sum[c];
      }
      const auto  grain  = FilmGrainMix(field.mix, FilmGrainNoise3{unit[0], unit[1], unit[2]});
      const auto& source = input[static_cast<std::size_t>(y) * width + x];
      const auto  result = FilmGrainPostPixel(&post, {source.r, source.g, source.b}, grain);
      output[static_cast<std::size_t>(y) * width + x] = {result.r, result.g, result.b, source.a};
    }
  }
  return output;
}

/// The largest channel difference of two images, and whether every alpha is the same.
inline auto LargestDifference(const std::vector<Rgba>& a, const std::vector<Rgba>& b) -> float {
  float largest = 0.0f;
  for (std::size_t i = 0; i < a.size(); ++i) {
    largest = std::max(
        {largest, std::abs(a[i].r - b[i].r), std::abs(a[i].g - b[i].g), std::abs(a[i].b - b[i].b)});
    EXPECT_EQ(a[i].a, b[i].a) << "pixel " << i;
  }
  return largest;
}

/**
 * Per-pixel standard deviation on a flat grey at 16mm, 35mm, 70mm and at 2560 and 6000 px: within
 * 5 % of the evaluator, and the lag-1 correlation within 0.03.
 */
inline void ExpectPixelSigmaAtGreyMatchesTheEvaluator(const Render&        render,
                                                      const FilmArrayFile& file) {
  const auto              sides         = file.I32("long_sides", {2});
  const auto              pixel_factor  = file.F64("pixel_factor", {3, 2, 3});
  const auto              post_pixel    = file.F64("post_pixel_sigma_linear", {3, 2, 3});
  const auto              post_value    = file.F64("post_display_value_linear", {3});
  const auto              post_sigma_48 = file.F64("post_display_sigma_48_linear", {3});
  const auto              lag1_kernel   = file.F64("lag1_kernel", {3, 2, 3});
  constexpr std::uint32_t kSize         = 512;
  for (std::size_t f = 0; f < kFormats.size(); ++f) {
    for (std::size_t s = 0; s < kLongSides.size(); ++s) {
      ASSERT_EQ(static_cast<std::uint32_t>(sides[s]), kLongSides[s]);
      const auto geometry = TileGeometry(kLongSides[s], 1.0f, 600.0f, 400.0f, kSize, kSize);
      const auto plan     = Plan(geometry, kFormats[f]);
      const auto input    = PostGrey(plan, geometry);
      const auto output   = render(plan, geometry, input, kSrgb);
      for (int channel = 0; channel < 3; ++channel) {
        SCOPED_TRACE(std::string(FilmFormatId(kFormats[f])) + " " + std::to_string(kLongSides[s]) +
                     " channel " + std::to_string(channel));
        const auto   index = (f * 2 + s) * 3 + static_cast<std::size_t>(channel);
        // Green is the evaluator value of the post path. Red and blue have the #660 ratio of the
        // scans at the 48 um aperture, at the pixel factor of the evaluator for their layer.
        const double expected =
            channel == 1 ? post_pixel[index] / post_value[1]
                         : kFilmGrainPostChannelRatio[static_cast<std::size_t>(channel)] *
                               post_sigma_48[1] / post_value[1] * pixel_factor[index];
        const auto change = DensityChange(input, output, channel);
        EXPECT_NEAR(StdDev(change) / expected, 1.0, 0.05);
        // The lag-1 of the mixed field: sum over k of L_jk^2 times the lag-1 of layer k.
        double lag1 = 0.0;
        for (std::size_t k = 0; k < 3; ++k) {
          const double l = plan.mix[static_cast<std::size_t>(channel) * 3 + k];
          lag1 += l * l * lag1_kernel[(f * 2 + s) * 3 + k];
        }
        EXPECT_NEAR(HorizontalLag1(change, kSize), lag1, 0.03);
      }
    }
  }
}

/// Issue 674: a render at scale 0.5 against the full render box-downscaled by 2: standard
/// deviation within 5 %, pattern correlation above 0.9.
inline void ExpectHalfScaleRenderHasTheGrainOfTheFullRenderBoxDownscaled(const Render& render) {
  constexpr std::uint32_t kFull = 1024;
  for (const auto format : {FilmFormat::Film16mm, FilmFormat::Film35mm}) {
    const auto full_geometry = TileGeometry(6000, 1.0f, 512.0f, 512.0f, kFull, kFull);
    const auto half_geometry = TileGeometry(6000, 2.0f, 512.0f, 512.0f, kFull / 2, kFull / 2);
    const auto full_plan     = Plan(full_geometry, format);
    const auto half_plan     = Plan(half_geometry, format);
    ASSERT_EQ(half_plan.aggregate, 2U);
    const auto grey        = FlatSrgb(static_cast<float>(full_plan.post_grey));
    const auto full_input  = MakeDisplay(full_geometry, grey);
    const auto half_input  = MakeDisplay(half_geometry, grey);
    const auto full_output = render(full_plan, full_geometry, full_input, kSrgb);
    const auto half_output = render(half_plan, half_geometry, half_input, kSrgb);
    for (int channel = 0; channel < 3; ++channel) {
      SCOPED_TRACE(std::string(FilmFormatId(format)) + " channel " + std::to_string(channel));
      const auto boxed = BoxHalf(DensityChange(full_input, full_output, channel), kFull, kFull);
      const auto half  = DensityChange(half_input, half_output, channel);
      EXPECT_NEAR(StdDev(half) / StdDev(boxed), 1.0, 0.05);
      EXPECT_GT(Correlation(half, boxed), 0.9);
    }
  }
}

/// Issue 687: a 512 x 512 viewport region equals the same region of the full render within 1e-5.
/// The region origin is not a multiple of the 16 x 16 tile, so the tiles of the two renders do not
/// line up and the halo of a tile edge must read the noise of the neighbour tile.
inline void ExpectViewportRegionEqualsTheSameRegionOfTheFullRender(const Render& render) {
  constexpr std::uint32_t kFull   = 1024;
  constexpr std::uint32_t kRegion = 512;
  constexpr std::uint32_t kLeft   = 250;
  constexpr std::uint32_t kTop    = 317;
  const auto              ramp    = [](float rx, float ry) {
    const float value = 0.15f + 0.6f * rx / 1024.0f;
    return Rgba{value, value * 0.9f + 0.05f * ry / 1024.0f, value * 0.8f, 1.0f};
  };
  const auto full_geometry   = TileGeometry(6000, 1.0f, 0.0f, 0.0f, kFull, kFull);
  const auto region_geometry = TileGeometry(6000, 1.0f, static_cast<float>(kLeft),
                                            static_cast<float>(kTop), kRegion, kRegion);
  const auto full_output     = render(Plan(full_geometry, FilmFormat::Film16mm), full_geometry,
                                      MakeDisplay(full_geometry, ramp), kSrgb);
  const auto region_output   = render(Plan(region_geometry, FilmFormat::Film16mm), region_geometry,
                                      MakeDisplay(region_geometry, ramp), kSrgb);
  float      largest         = 0.0f;
  for (std::uint32_t y = 0; y < kRegion; ++y) {
    for (std::uint32_t x = 0; x < kRegion; ++x) {
      const auto& a = region_output[static_cast<std::size_t>(y) * kRegion + x];
      const auto& b = full_output[static_cast<std::size_t>(y + kTop) * kFull + x + kLeft];
      largest = std::max({largest, std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b)});
    }
  }
  EXPECT_LE(largest, 1.0e-5f);
}

/// The colour of the grain at the grey: the channel correlation of the 5207 scans of #660, and the
/// quietest green.
inline void ExpectChannelCorrelationAtGreyIsTheScanCorrelationOf660(const Render& render) {
  constexpr std::uint32_t kSize    = 512;
  const auto              geometry = TileGeometry(6000, 1.0f, 300.0f, 300.0f, kSize, kSize);
  const auto              plan     = Plan(geometry, FilmFormat::Film35mm);
  const auto              input    = PostGrey(plan, geometry);
  const auto              output   = render(plan, geometry, input, kSrgb);
  const auto              r        = DensityChange(input, output, 0);
  const auto              g        = DensityChange(input, output, 1);
  const auto              b        = DensityChange(input, output, 2);
  EXPECT_NEAR(Correlation(r, g), kFilmGrainPostScanCorrelation[0], 0.03);
  EXPECT_NEAR(Correlation(g, b), kFilmGrainPostScanCorrelation[1], 0.03);
  EXPECT_NEAR(Correlation(r, b), kFilmGrainPostScanCorrelation[2], 0.03);
  EXPECT_LT(StdDev(g), StdDev(r));
  EXPECT_LT(StdDev(g), StdDev(b));
}

/// The same change of linear light for an ST 2084 output as for an sRGB output.
inline void ExpectSt2084OutputGetsTheSameChangeOfLinearLightAsSrgbOutput(const Render& render) {
  constexpr std::uint32_t kSize    = 256;
  const auto              geometry = TileGeometry(6000, 1.0f, 300.0f, 300.0f, kSize, kSize);
  const auto              plan     = Plan(geometry, FilmFormat::Film35mm);
  const float             grey     = static_cast<float>(plan.post_grey);
  const auto              srgb_in  = MakeDisplay(geometry, FlatSrgb(grey));
  // The same light in ST 2084: 1.0 relative is the reference white of 100 nits.
  const float             pq_code  = CePqEncode(grey * 100.0f / CE_PQ_PEAK_NITS);
  const auto              pq_in    = MakeDisplay(
      geometry, [pq_code](float, float) { return Rgba{pq_code, pq_code, pq_code, 1.0f}; });
  const FilmGrainDisplayEncoding pq{CE_TF_ST2084, CE_PQ_PEAK_NITS / 100.0f};
  const auto                     srgb_out = render(plan, geometry, srgb_in, kSrgb);
  const auto                     pq_out   = render(plan, geometry, pq_in, pq);
  for (int channel = 0; channel < 3; ++channel) {
    const auto srgb_change = DensityChange(srgb_in, srgb_out, channel);
    double     largest     = 0.0;
    for (std::size_t i = 0; i < pq_out.size(); ++i) {
      const double pq_change =
          -std::log(static_cast<double>(CePqDecode(Channel(pq_out[i], channel))) /
                    static_cast<double>(CePqDecode(pq_code)));
      largest = std::max(largest, std::abs(pq_change - srgb_change[i]));
    }
    EXPECT_LT(largest, 2.0e-3) << "channel " << channel;
  }
}

/// 16mm at 6000 px rendered at half size: a render of 37 x 29 with partial tiles at the right and
/// the bottom, and a fit-view aggregate of 2, against the host form of the steps.
inline void ExpectKernelEqualsTheHostFormOfItsStepsAtTheTileEdges(const Render& render) {
  constexpr std::uint32_t kWidth   = 37;
  constexpr std::uint32_t kHeight  = 29;
  const auto              geometry = TileGeometry(6000, 2.0f, 101.0f, 57.0f, kWidth, kHeight);
  const auto              plan     = Plan(geometry, FilmFormat::Film16mm, 1.6f);
  ASSERT_GE(MakeFilmGrainFieldParams(plan, geometry).halo, 1);
  const auto ramp = [](float rx, float ry) {
    const float value = 0.1f + 0.7f * (rx - 101.0f) / 74.0f;
    return Rgba{value, value * 0.85f + 0.002f * (ry - 57.0f), value * 0.7f, 1.0f};
  };
  const auto input = MakeDisplay(geometry, ramp);
  EXPECT_LE(
      LargestDifference(render(plan, geometry, input, kSrgb), HostForm(plan, geometry, input)),
      2.0e-5f);
}

/// 16mm at the smallest reference long side of 6000 x 2^n px whose blur radius is the limit 24, so
/// the local memory of a tile holds its largest form (4 bands of noise on OpenCL and Metal). The
/// render scale of the plan is at most 1, so only a smaller reference pitch makes the radius
/// larger.
inline void ExpectLargestBlurRadiusEqualsTheHostForm(const Render& render) {
  constexpr std::uint32_t kSize     = 40;
  std::uint32_t           long_side = 6000;
  ResolvedRenderGeometry  geometry;
  FilmGrainPlan           plan;
  do {
    long_side *= 2U;
    geometry = TileGeometry(long_side, 1.0f, 900.0f, 700.0f, kSize, kSize);
    plan     = Plan(geometry, FilmFormat::Film16mm);
  } while (MakeFilmGrainFieldParams(plan, geometry).halo < FILM_GRAIN_MAX_BLUR_RADIUS &&
           long_side < 6000U * 1024U);
  ASSERT_EQ(MakeFilmGrainFieldParams(plan, geometry).halo, FILM_GRAIN_MAX_BLUR_RADIUS);
  const auto input = PostGrey(plan, geometry);
  EXPECT_LE(
      LargestDifference(render(plan, geometry, input, kSrgb), HostForm(plan, geometry, input)),
      2.0e-5f);
}

/// The standard deviation of the grain of @p render against @p reference at a flat grey, for each
/// format at 6000 px: within @p tolerance (relative) for each channel.
inline void ExpectSigmaAtGreyMatches(const Render& render, const Render& reference,
                                     double tolerance) {
  constexpr std::uint32_t kSize = 256;
  for (const auto format : kFormats) {
    const auto geometry = TileGeometry(6000, 1.0f, 700.0f, 500.0f, kSize, kSize);
    const auto plan     = Plan(geometry, format);
    const auto input    = PostGrey(plan, geometry);
    const auto actual   = render(plan, geometry, input, kSrgb);
    const auto expected = reference(plan, geometry, input, kSrgb);
    for (int channel = 0; channel < 3; ++channel) {
      SCOPED_TRACE(std::string(FilmFormatId(format)) + " channel " + std::to_string(channel));
      const double sigma           = StdDev(DensityChange(input, actual, channel));
      const double reference_sigma = StdDev(DensityChange(input, expected, channel));
      EXPECT_NEAR(sigma / reference_sigma, 1.0, tolerance);
    }
  }
}

}  // namespace alcedo::film_grain_post_test
