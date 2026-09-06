//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "cached_versus_fresh_pixel_fixture.hpp"

#include <gtest/gtest.h>

#include <limits>

#include "../input/prepared_raw_test_support.hpp"

namespace alcedo {
namespace {

TEST(CachedVersusFreshPixelFixture,
     IndependentExposureMatchesAcesccFormulaOnDeterministicRamp) {
  const auto input    = cached_result_pixel::MakeDeterministicRgbRamp();
  const auto expected = cached_result_pixel::ApplyIndependentExposure(input, 1.0f);
  ASSERT_EQ(expected.size(), static_cast<std::size_t>(cached_result_pixel::kFixtureWidth) *
                                 cached_result_pixel::kFixtureHeight);
  const auto& first = input.front();
  EXPECT_NEAR(expected.front().r, multi_grade_test::ApplyExposureAcescc(first.r, 1.0f),
              cached_result_pixel::PixelCompareRule::kMaxAbsTolerance);
  EXPECT_NEAR(expected.front().g, multi_grade_test::ApplyExposureAcescc(first.g, 1.0f),
              cached_result_pixel::PixelCompareRule::kMaxAbsTolerance);
  EXPECT_NEAR(expected.front().b, multi_grade_test::ApplyExposureAcescc(first.b, 1.0f),
              cached_result_pixel::PixelCompareRule::kMaxAbsTolerance);
  EXPECT_FLOAT_EQ(expected.front().a, 1.0f);
}

TEST(CachedVersusFreshPixelFixture,
     RepeatedIndependentExposureAgreesWithItselfAndWithPackedPlaneLayout) {
  const auto packed = gpu_dag_test::MakeF32RgbaPlane(cached_result_pixel::kFixtureWidth,
                                                     cached_result_pixel::kFixtureHeight);
  const auto* packed_px = reinterpret_cast<const float*>(packed.bytes.get());
  const auto ramp       = cached_result_pixel::MakeDeterministicRgbRamp();
  ASSERT_EQ(ramp.size() * 4U, packed.extent.width * packed.extent.height * 4U);
  EXPECT_FLOAT_EQ(ramp.front().r, packed_px[0]);
  EXPECT_FLOAT_EQ(ramp.front().g, packed_px[1]);
  EXPECT_FLOAT_EQ(ramp.front().b, packed_px[2]);

  const auto first  = cached_result_pixel::ApplyIndependentExposure(ramp, -0.5f);
  const auto second = cached_result_pixel::ApplyIndependentExposure(ramp, -0.5f);
  const auto same   = cached_result_pixel::CompareRgba(first, second);
  EXPECT_FALSE(same.size_mismatch);
  EXPECT_FALSE(same.saw_non_finite);
  EXPECT_TRUE(same.passed) << "max_abs_error=" << same.max_abs_error;
}

TEST(CachedVersusFreshPixelFixture, NonFiniteSampleFailsComparisonWithoutTuningTolerance) {
  auto left  = cached_result_pixel::MakeDeterministicRgbRamp(2, 1);
  auto right = left;
  right[0].r = std::numeric_limits<float>::quiet_NaN();
  const auto result = cached_result_pixel::CompareRgba(left, right);
  EXPECT_TRUE(result.saw_non_finite);
  EXPECT_FALSE(result.passed);
}

}  // namespace
}  // namespace alcedo
