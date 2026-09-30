//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <cmath>
#include <stdexcept>

#include "edit/graph/diffusion_filter_model.hpp"
#include "edit/runtime/diffusion_filter_plan.hpp"

namespace alcedo {
namespace {

auto WeightSum(const DiffusionFilterLayout& layout) -> double {
  double sum = 0.0;
  for (std::uint32_t index = 0; index < layout.level_count; ++index) {
    sum += layout.weights[index];
  }
  return sum;
}

TEST(DiffusionFilterPlan, FullResolutionLayoutUsesExpectedBaseLevelAndLevelCount) {
  // 6008 x 4008: sigma_base = 8.016 px (level 3), sigma_max = 481 px -> 7 levels.
  const auto layout =
      MakeDiffusionFilterLayout({6008, 4008}, 4008.0f, ResolveDiffusionFilterShape(0.5f));
  EXPECT_EQ(layout.base_level, 3U);
  EXPECT_EQ(layout.level_count, 7U);
  EXPECT_EQ(layout.extents[0].width, 751U);
  EXPECT_EQ(layout.extents[0].height, 501U);
  EXPECT_EQ(layout.extents[6].width, 12U);
  EXPECT_EQ(layout.extents[6].height, 8U);
}

TEST(DiffusionFilterPlan, WeightsSumToOneAndFollowThePowerLawWithoutBlackMist) {
  auto shape       = ResolveDiffusionFilterShape(1.0f);
  shape.black_mist = 0.0f;
  const auto layout = MakeDiffusionFilterLayout({4000, 3000}, 3000.0f, shape);
  EXPECT_NEAR(WeightSum(layout), 1.0, 1e-6);
  ASSERT_GE(layout.level_count, 3U);
  // w_k is proportional to sigma_k^(2 - p) and sigma doubles per level.
  const double ratio = std::pow(2.0, 2.0 - shape.power_law_exponent);
  EXPECT_NEAR(layout.weights[1] / layout.weights[0], ratio, 1e-5);
  EXPECT_NEAR(layout.weights[2] / layout.weights[1], ratio, 1e-5);
}

TEST(DiffusionFilterPlan, BlackMistReducesTheWidestLevelAndTransmission) {
  auto clear       = ResolveDiffusionFilterShape(1.0f);
  clear.black_mist = 0.0f;
  auto misted      = clear;
  misted.black_mist = 1.0f;
  const auto clear_layout  = MakeDiffusionFilterLayout({2000, 1500}, 1500.0f, clear);
  const auto misted_layout = MakeDiffusionFilterLayout({2000, 1500}, 1500.0f, misted);
  ASSERT_EQ(clear_layout.level_count, misted_layout.level_count);
  const auto last = misted_layout.level_count - 1;
  EXPECT_EQ(misted_layout.weights[last], 0.0f);
  EXPECT_GT(clear_layout.weights[last], 0.0f);
  EXPECT_NEAR(WeightSum(misted_layout), 1.0, 1e-6);
  EXPECT_EQ(clear_layout.transmission, 1.0f);
  EXPECT_LT(misted_layout.transmission, 1.0f);
}

TEST(DiffusionFilterPlan, PreviewAndExportUseTheSameImageSpaceGlow) {
  const auto shape   = ResolveDiffusionFilterShape(0.5f);
  const auto full    = MakeDiffusionFilterLayout({6000, 4000}, 4000.0f, shape);
  const auto preview = MakeDiffusionFilterLayout({1500, 1000}, 1000.0f, shape);
  // A 1/4-scale render starts two levels lower and covers the same image fraction.
  EXPECT_EQ(full.base_level, preview.base_level + 2U);
  EXPECT_EQ(full.level_count, preview.level_count);
}

TEST(DiffusionFilterPlan, TinyRenderStopsAtTheFirstSinglePixelLevel) {
  const auto layout =
      MakeDiffusionFilterLayout({3, 2}, 2000.0f, ResolveDiffusionFilterShape(1.0f));
  ASSERT_GE(layout.level_count, 1U);
  const auto last = layout.extents[layout.level_count - 1];
  EXPECT_EQ(last.width, 1U);
  EXPECT_EQ(last.height, 1U);
  EXPECT_NEAR(WeightSum(layout), 1.0, 1e-6);
}

TEST(DiffusionFilterPlan, EmptyExtentOrShortSideIsRejected) {
  const auto shape = ResolveDiffusionFilterShape(0.5f);
  EXPECT_THROW((void)MakeDiffusionFilterLayout({0, 10}, 10.0f, shape), std::invalid_argument);
  EXPECT_THROW((void)MakeDiffusionFilterLayout({10, 10}, 0.0f, shape), std::invalid_argument);
}

}  // namespace
}  // namespace alcedo
