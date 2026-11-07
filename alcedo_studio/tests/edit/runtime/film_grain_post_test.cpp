//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Host tests of the post path of the film grain (plan 'film_grain_format_and_stock_plan.md', phase
// G3): the post target of the plan, the kernel parameters, the reference noise with its aggregate,
// and the apply step of film_grain_post.h.

#include "edit/runtime/film/film_grain_post.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <stdexcept>
#include <vector>

#include "color/color_encoding_math.h"
#include "edit/geometry/render_geometry_resolver.hpp"
#include "edit/geometry/source_geometry.hpp"
#include "edit/runtime/film/film_grain_field.h"
#include "edit/runtime/film/film_stock_bundle.hpp"
#include "edit/runtime/film_grain_plan.hpp"

namespace alcedo {
namespace {

/// Grain data with the constants of the shipped bundle (lab step P15). The curves have no physical
/// meaning; each has 4 nodes.
struct PostStockValues {
  std::array<double, 3>       cloud_sigma_um{2.0, 2.0, 2.8};
  std::array<double, 3>       layer_correlation{0.489, 0.2349, 0.1743};
  std::array<double, 3>       post_value{0.188, 0.191, 0.172};
  std::array<double, 3>       post_sigma_48{0.0107, 0.0100, 0.0152};
  std::array<double, 3>       post_correlation{0.535, 0.509, 0.087};
  std::vector<double>         x{0.2, 0.8, 1.4, 2.4, 0.3, 0.9, 1.5, 2.5, 1.0, 1.3, 2.0, 3.0};
  std::vector<double>         sigma{0.0056, 0.0108, 0.0084, 0.0048, 0.0052, 0.0108,
                            0.0082, 0.0062, 0.0118, 0.0158, 0.0110, 0.0042};
  std::array<std::int32_t, 3> count{4, 4, 4};

  [[nodiscard]] auto          View() const -> FilmGrainStockView {
    return {
                 .layer   = {x, sigma, count, 4},
                 .channel = {x, sigma, count, 4},
                 .common  = {.cloud_sigma_um           = cloud_sigma_um,
                             .layer_correlation        = layer_correlation,
                             .post_display_value       = post_value,
                             .post_display_sigma_48    = post_sigma_48,
                             .post_display_correlation = post_correlation,
                             .aperture_area_um2        = std::numbers::pi * 24.0 * 24.0},
                 .tier    = "curve",
    };
  }
};

auto Frame(std::uint32_t long_side, float render_scale = 1.0f) -> ResolvedRenderGeometry {
  const Extent2D    extent{long_side, long_side * 2U / 3U};
  ResolutionRequest resolution;
  resolution.render_scale = render_scale;
  return ResolveRenderGeometry(MakeSourceGeometry(extent, extent), {}, {}, resolution, {});
}

auto PostPlan(float strength = 1.0f, FilmFormat format = FilmFormat::Film35mm,
              float render_scale = 1.0f) -> FilmGrainPlan {
  const PostStockValues stock;
  return MakeFilmGrainPlan(Frame(6000, render_scale), format, stock.View(), FilmGrainTarget::Post,
                           strength);
}

auto Grey(float linear) -> FilmGrainPostRgb {
  const float code = CeSrgbEncode(linear);
  return {code, code, code};
}

auto Unit(float value) -> FilmGrainNoise3 { return {value, value, value}; }

/// -ln(y_out / y_in) of one channel of an sRGB pixel.
auto DensityChange(float code_in, float code_out) -> double {
  return -std::log(static_cast<double>(CeSrgbDecode(code_out)) /
                   static_cast<double>(CeSrgbDecode(code_in)));
}

TEST(FilmGrainPostPlan, PostTargetIsTheGreenOfTheBundleWithTheScanRatio) {
  const auto plan = PostPlan();
  EXPECT_DOUBLE_EQ(plan.post_grey, 0.191);
  for (std::size_t channel = 0; channel < 3; ++channel) {
    EXPECT_NEAR(plan.post_relative_sigma_48[channel],
                0.0100 / 0.191 * kFilmGrainPostChannelRatio[channel], 1e-15);
  }
  EXPECT_DOUBLE_EQ(plan.curve_low[2], 1.0);
  EXPECT_DOUBLE_EQ(plan.curve_high[2], 3.0);
}

TEST(FilmGrainPostPlan, PostMixGivesTheScanCorrelationOf660) {
  const auto& l = PostPlan().mix;
  // L L^T: rg is row 1 against row 0, gb row 2 against row 1, rb row 2 against row 0.
  EXPECT_NEAR(l[3] * l[0], 0.661, 1e-12);
  EXPECT_NEAR(l[6] * l[3] + l[7] * l[4], 0.41, 1e-12);
  EXPECT_NEAR(l[6] * l[0], 0.246, 1e-12);
}

TEST(FilmGrainPostPlan, FieldParamsHoldTheKernelsTheMixAndTheReferenceMapping) {
  const auto geometry = Frame(6000, 0.5f);
  const auto plan     = PostPlan(1.0f, FilmFormat::Film16mm, 0.5f);
  const auto params   = MakeFilmGrainFieldParams(plan, geometry);
  EXPECT_EQ(params.noise.seed, kFilmGrainSeed);
  EXPECT_EQ(params.noise.aggregate, 2U);
  EXPECT_FLOAT_EQ(params.noise.inverse_aggregate, 0.5f);
  for (std::size_t entry = 0; entry < 6; ++entry) {
    EXPECT_FLOAT_EQ(params.noise.render_to_reference[entry], geometry.render_to_reference.m[entry]);
  }
  int halo = 0;
  for (std::size_t layer = 0; layer < 3; ++layer) {
    EXPECT_EQ(params.radius[layer], static_cast<int>(plan.layers[layer].radius));
    halo = std::max(halo, params.radius[layer]);
    for (std::size_t tap = 0; tap <= kFilmGrainMaxBlurRadius; ++tap) {
      EXPECT_FLOAT_EQ(params.weights[layer][tap],
                      static_cast<float>(plan.layers[layer].weights[tap]));
    }
    EXPECT_FLOAT_EQ(params.inverse_weight_square_sum[layer],
                    static_cast<float>(1.0 / plan.layers[layer].weight_square_sum));
  }
  EXPECT_EQ(params.halo, halo);
  const std::array<std::size_t, 6> lower{0, 3, 4, 6, 7, 8};
  for (std::size_t entry = 0; entry < 6; ++entry) {
    EXPECT_FLOAT_EQ(params.mix[entry], static_cast<float>(plan.mix[lower[entry]]));
  }
}

TEST(FilmGrainPostField, AggregateNoiseIsTheScaledSumOfTheReferenceGaussians) {
  // A render at half size from reference pixel (100, 40): render pixel (x, y) reads the reference
  // pixels 2x + 100 and 2x + 101, 2y + 40 and 2y + 41.
  FilmGrainNoiseParams params{{2.0f, 0.0f, 100.0f, 0.0f, 2.0f, 40.0f}, kFilmGrainSeed, 2U, 0.5f};
  for (const int x : {-3, 0, 7}) {
    for (const int y : {-2, 0, 5}) {
      float sum = 0.0f;
      for (int j = 0; j < 2; ++j) {
        for (int i = 0; i < 2; ++i) {
          const auto bits = FilmGrainHash3(static_cast<unsigned>(2 * x + 100 + i),
                                           static_cast<unsigned>(2 * y + 40 + j), kFilmGrainSeed);
          sum += FilmGrainGaussian(bits.y);
        }
      }
      EXPECT_FLOAT_EQ(FilmGrainAggregateNoise(params, x, y).y, 0.5f * sum) << x << " " << y;
    }
  }
  // At full size, a render pixel reads its own reference pixel.
  FilmGrainNoiseParams full{{1.0f, 0.0f, 64.0f, 0.0f, 1.0f, 64.0f}, kFilmGrainSeed, 1U, 1.0f};
  const auto           bits = FilmGrainHash3(64U + 3U, 64U + 9U, kFilmGrainSeed);
  EXPECT_FLOAT_EQ(FilmGrainAggregateNoise(full, 3, 9).z, FilmGrainGaussian(bits.z));
}

TEST(FilmGrainPostField, AggregateNoiseHasUnitVarianceAtEachK) {
  for (const std::uint32_t k : {1U, 2U, 3U, 4U}) {
    const float          scale = static_cast<float>(k);
    FilmGrainNoiseParams params{
        {scale, 0.0f, 0.0f, 0.0f, scale, 0.0f}, kFilmGrainSeed, k, 1.0f / scale};
    double        sum = 0.0, square = 0.0;
    constexpr int kSide = 300;
    for (int y = 0; y < kSide; ++y) {
      for (int x = 0; x < kSide; ++x) {
        const double value = FilmGrainAggregateNoise(params, x, y).x;
        sum += value;
        square += value * value;
      }
    }
    const double count = static_cast<double>(kSide) * kSide;
    EXPECT_NEAR(square / count - (sum / count) * (sum / count), 1.0, 0.02) << k;
  }
}

TEST(FilmGrainPostApply, UnitGrainAtTheGreyGivesTheTargetTimesThePixelFactor) {
  const auto                 plan   = PostPlan(1.3f);
  const auto                 params = MakeFilmGrainPostParams(plan, CE_TF_SRGB, 1.0f);
  const auto                 source = Grey(static_cast<float>(plan.post_grey));
  const auto                 result = FilmGrainPostPixel(&params, source, Unit(1.0f));
  const std::array<float, 3> out{result.r, result.g, result.b};
  for (std::size_t channel = 0; channel < 3; ++channel) {
    const double expected =
        1.3 * plan.post_relative_sigma_48[channel] * plan.layers[channel].pixel_factor;
    EXPECT_NEAR(DensityChange(source.r, out[channel]) / expected, 1.0, 1e-4) << channel;
  }
}

TEST(FilmGrainPostApply, DensityChangeIsLinearInStrengthAndInTheGrain) {
  const auto source = Grey(0.3f);
  const auto half   = MakeFilmGrainPostParams(PostPlan(0.5f), CE_TF_SRGB, 1.0f);
  const auto full   = MakeFilmGrainPostParams(PostPlan(1.0f), CE_TF_SRGB, 1.0f);
  const auto one    = FilmGrainPostPixel(&half, source, Unit(0.8f));
  const auto two    = FilmGrainPostPixel(&full, source, Unit(0.8f));
  const auto twice  = FilmGrainPostPixel(&half, source, Unit(1.6f));
  EXPECT_GT(DensityChange(source.g, one.g), 0.0);
  EXPECT_NEAR(DensityChange(source.g, two.g), 2.0 * DensityChange(source.g, one.g), 1e-5);
  EXPECT_NEAR(DensityChange(source.g, twice.g), DensityChange(source.g, two.g), 1e-5);
}

TEST(FilmGrainPostApply, BlackAndReferenceWhiteGetNoGrain) {
  const auto params = MakeFilmGrainPostParams(PostPlan(2.0f), CE_TF_SRGB, 1.0f);
  for (const float code : {0.0f, -0.02f, 1.0f, 1.2f}) {
    const auto result = FilmGrainPostPixel(&params, {code, code, code}, Unit(1.5f));
    EXPECT_FLOAT_EQ(result.r, code) << code;
    EXPECT_FLOAT_EQ(result.g, code) << code;
    EXPECT_FLOAT_EQ(result.b, code) << code;
  }
}

TEST(FilmGrainPostApply, BrighteningGrainIsSmallerThanDarkeningGrainNearWhite) {
  const auto params  = MakeFilmGrainPostParams(PostPlan(1.0f), CE_TF_SRGB, 1.0f);
  const auto source  = Grey(0.75f);
  const auto darker  = FilmGrainPostPixel(&params, source, Unit(1.0f));
  const auto lighter = FilmGrainPostPixel(&params, source, Unit(-1.0f));
  EXPECT_GT(DensityChange(source.g, darker.g), 0.0);
  EXPECT_LT(DensityChange(source.g, lighter.g), 0.0);
  EXPECT_LT(-DensityChange(source.g, lighter.g), 0.5 * DensityChange(source.g, darker.g));
  // In the mid tones both halves have the same size.
  const auto mid      = Grey(0.19f);
  const auto mid_dark = FilmGrainPostPixel(&params, mid, Unit(1.0f));
  const auto mid_lite = FilmGrainPostPixel(&params, mid, Unit(-1.0f));
  EXPECT_NEAR(-DensityChange(mid.g, mid_lite.g), DensityChange(mid.g, mid_dark.g), 1e-5);
}

TEST(FilmGrainPostApply, LinearAndSrgbOutputGetTheSameChangeOfLight) {
  const auto plan   = PostPlan(1.0f);
  const auto srgb   = MakeFilmGrainPostParams(plan, CE_TF_SRGB, 1.0f);
  const auto linear = MakeFilmGrainPostParams(plan, CE_TF_LINEAR, 1.0f);
  for (const float light : {0.05f, 0.19f, 0.5f}) {
    const auto encoded = FilmGrainPostPixel(&srgb, Grey(light), Unit(0.7f));
    const auto direct  = FilmGrainPostPixel(&linear, {light, light, light}, Unit(0.7f));
    EXPECT_NEAR(CeSrgbDecode(encoded.b), direct.b, 2e-6f * (1.0f + light)) << light;
  }
}

TEST(FilmGrainPostPlan, PostParamsRejectANegativePlanAndABadDecodeScale) {
  const PostStockValues stock;
  const auto negative = MakeFilmGrainPlan(Frame(6000), FilmFormat::Film35mm, stock.View(),
                                          FilmGrainTarget::Negative, 1.0f);
  EXPECT_THROW((void)MakeFilmGrainPostParams(negative, CE_TF_SRGB, 1.0f), std::invalid_argument);
  EXPECT_THROW((void)MakeFilmGrainPostParams(PostPlan(), CE_TF_SRGB, 0.0f), std::invalid_argument);
}

}  // namespace
}  // namespace alcedo
