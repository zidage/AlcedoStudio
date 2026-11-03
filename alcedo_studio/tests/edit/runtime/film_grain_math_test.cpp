//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/film/film_grain_math.h"

#include <gtest/gtest.h>

#include <random>
#include <vector>

#include "film_grain_colour_statistics.hpp"

namespace alcedo {
namespace {

using film_grain_test::GrainResidualColour;
using film_grain_test::MeasureGrainResidualColour;

constexpr float kAmount = 0.6f / 3.0f;

/// Applies the dye clouds to a flat grey with independent, equal-variance coverage deviations,
/// the statistics of the three blurred Bernoulli fields of the GPU passes.
auto            ResidualColourAtGrey(float grey, float amount = kAmount) -> GrainResidualColour {
  std::mt19937                    rng(20261103U);
  std::normal_distribution<float> deviation(0.0f, 0.08f);
  std::vector<FilmGrainRgb>       input;
  std::vector<FilmGrainRgb>       output;
  constexpr int                   kSamples = 200000;
  input.reserve(kSamples);
  output.reserve(kSamples);
  const FilmGrainRgb source{grey, grey, grey};
  for (int i = 0; i < kSamples; ++i) {
    const FilmGrainRgb coverage{grey + deviation(rng), grey + deviation(rng),
                                grey + deviation(rng)};
    input.push_back(source);
    output.push_back(FilmGrainDyeClouds(source, coverage, amount));
  }
  return MeasureGrainResidualColour(input, output);
}

TEST(FilmGrainMath, DyeCloudsLeaveTheValueWhenCoverageEqualsTheValue) {
  for (const float value : {0.05f, 0.2f, 0.35f, 0.5f, 0.8f, 0.95f}) {
    const FilmGrainRgb source{value, value * 0.9f, value * 0.7f};
    const FilmGrainRgb result = FilmGrainDyeClouds(source, source, kAmount);
    EXPECT_FLOAT_EQ(result.r, source.r) << value;
    EXPECT_FLOAT_EQ(result.g, source.g) << value;
    EXPECT_FLOAT_EQ(result.b, source.b) << value;
  }
}

TEST(FilmGrainMath, DyeCloudsAddNoGrainAtBlackAndWhite) {
  for (const float value : {0.0f, 1.0f}) {
    const FilmGrainRgb source{value, value, value};
    const FilmGrainRgb coverage{0.5f, 0.2f, 0.9f};
    const FilmGrainRgb result = FilmGrainDyeClouds(source, coverage, kAmount);
    EXPECT_FLOAT_EQ(result.r, value);
    EXPECT_FLOAT_EQ(result.g, value);
    EXPECT_FLOAT_EQ(result.b, value);
  }
}

TEST(FilmGrainMath, DyeCloudChangeScalesLinearlyWithAmountBelowTheHighlights) {
  const FilmGrainRgb source{0.4f, 0.35f, 0.3f};
  const FilmGrainRgb coverage{0.52f, 0.21f, 0.33f};
  const FilmGrainRgb single = FilmGrainDyeClouds(source, coverage, kAmount);
  const FilmGrainRgb twice  = FilmGrainDyeClouds(source, coverage, 2.0f * kAmount);
  EXPECT_NEAR(twice.r - source.r, 2.0f * (single.r - source.r), 1.0e-6f);
  EXPECT_NEAR(twice.g - source.g, 2.0f * (single.g - source.g), 1.0e-6f);
  EXPECT_NEAR(twice.b - source.b, 2.0f * (single.b - source.b), 1.0e-6f);
}

TEST(FilmGrainMath, DyeCloudChannelCorrelationIsTheMeasuredLayerMix) {
  // The mix is the square root of the target correlation, so independent fields come out with it.
  const auto colour = ResidualColourAtGrey(0.35f);
  EXPECT_NEAR(colour.correlation_rg, 0.4231, 0.02);
  EXPECT_NEAR(colour.correlation_gb, 0.4231, 0.02);
  EXPECT_NEAR(colour.correlation_rb, 0.2322, 0.02);
}

TEST(FilmGrainMath, DyeCloudChromaIsMostlyWarmCoolAndGreenIsTheQuietestChannel) {
  // Measured on Kodak 5207 and Pro Image 100: the main chroma axis is warm-cool, and the green
  // grain is the smallest. The previous mix had green larger than red and a green-magenta axis.
  for (const float grey : {0.2f, 0.35f, 0.5f, 0.65f}) {
    const auto colour = ResidualColourAtGrey(grey);
    EXPECT_GT(colour.warm_cool_share, 1.8 * colour.green_magenta_share) << grey;
    EXPECT_GT(colour.neutral_share, 0.45) << grey;
    EXPECT_LT(colour.neutral_share, 0.7) << grey;
    EXPECT_LT(colour.std_dev[1], colour.std_dev[0]) << grey;
    EXPECT_LT(colour.std_dev[1], colour.std_dev[2]) << grey;
  }
}

TEST(FilmGrainMath, DyeCloudsKeepTheirColourNearWhite) {
  // The previous mix made the grain 0.9 neutral at a grey of 0.9. Real grain keeps its colour.
  const auto colour = ResidualColourAtGrey(0.88f);
  EXPECT_LT(colour.neutral_share, 0.7);
  EXPECT_GT(colour.warm_cool_share, colour.green_magenta_share);
}

TEST(FilmGrainMath, DyeCloudsBrightenLessThanTheyDarkenNearWhite) {
  const FilmGrainRgb source{0.92f, 0.92f, 0.92f};
  const FilmGrainRgb darker = FilmGrainDyeClouds(source, {1.0f, 1.0f, 1.0f}, kAmount);
  const FilmGrainRgb lighter =
      FilmGrainDyeClouds(source, {0.84f, 0.84f, 0.84f}, kAmount);  // the same deviation
  EXPECT_LT(darker.g, source.g);
  EXPECT_GT(lighter.g, source.g);
  EXPECT_LT(lighter.g - source.g, 0.5f * (source.g - darker.g));
}

}  // namespace
}  // namespace alcedo
