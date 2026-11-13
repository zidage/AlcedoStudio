//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "edit/scope/scope_analyzer.hpp"

namespace alcedo {
namespace {

constexpr int   kBins         = 256;
// Smallest plot of the editor scope slot, in logical px.
constexpr float kPlotHeightPx = 150.0f;

auto            Index(int channel, int bin) -> size_t {
  return static_cast<size_t>(channel) * static_cast<size_t>(kBins) + static_cast<size_t>(bin);
}

// 1,000,000 pixels per channel: 90% in bin 200 (a sky), 10% spread evenly over bins 20..119.
auto DominantSkyCounts() -> std::vector<uint32_t> {
  std::vector<uint32_t> counts(static_cast<size_t>(kBins) * 3U, 0U);
  for (int channel = 0; channel < 3; ++channel) {
    counts[Index(channel, 200)] = 900000U;
    for (int bin = 20; bin < 120; ++bin) {
      counts[Index(channel, bin)] = 1000U;
    }
  }
  return counts;
}

TEST(ScopeHistogramRenderDataTest, DominantBinKeepsTheOtherPopulatedBinsReadable) {
  const auto data = BuildScopeHistogramRenderData(DominantSkyCounts(), kBins);
  ASSERT_TRUE(data.valid);
  ASSERT_EQ(data.rgb.size(), static_cast<size_t>(kBins) * 3U);
  ASSERT_EQ(data.peak_cut.size(), data.rgb.size());

  for (int channel = 0; channel < 3; ++channel) {
    // A linear scale against the peak shows these bins at 1000 / 900000 = 0.17 px.
    for (int bin = 20; bin < 120; ++bin) {
      EXPECT_GE(data.rgb[Index(channel, bin)] * kPlotHeightPx, 40.0f) << "bin " << bin;
      EXPECT_EQ(data.peak_cut[Index(channel, bin)], 0U) << "bin " << bin;
    }
    EXPECT_FLOAT_EQ(data.rgb[Index(channel, 200)], 1.0f);
    EXPECT_EQ(data.peak_cut[Index(channel, 200)], 1U);
  }
}

TEST(ScopeHistogramRenderDataTest, EmptyBinsStayAtZero) {
  const auto data = BuildScopeHistogramRenderData(DominantSkyCounts(), kBins);
  ASSERT_TRUE(data.valid);
  for (int channel = 0; channel < 3; ++channel) {
    for (int bin : {0, 19, 120, 199, 201, 255}) {
      EXPECT_EQ(data.rgb[Index(channel, bin)], 0.0f) << "bin " << bin;
      EXPECT_EQ(data.peak_cut[Index(channel, bin)], 0U) << "bin " << bin;
    }
  }
}

TEST(ScopeHistogramRenderDataTest, SinglePixelBinShowsAboveTheBaseline) {
  auto counts           = DominantSkyCounts();
  counts[Index(1, 160)] = 1U;
  const auto data       = BuildScopeHistogramRenderData(counts, kBins);
  ASSERT_TRUE(data.valid);
  EXPECT_GE(data.rgb[Index(1, 160)] * kPlotHeightPx, 2.0f);
  EXPECT_LT(data.rgb[Index(1, 160)], data.rgb[Index(1, 50)]);
}

TEST(ScopeHistogramRenderDataTest, SmoothDistributionTouchesTheTopWithoutACut) {
  std::vector<uint32_t> counts(static_cast<size_t>(kBins) * 3U, 0U);
  for (int channel = 0; channel < 3; ++channel) {
    for (int bin = 0; bin < kBins; ++bin) {
      const double offset         = (static_cast<double>(bin) - 128.0) / 40.0;
      counts[Index(channel, bin)] = static_cast<uint32_t>(10000.0 * std::exp(-offset * offset));
    }
  }
  const auto data = BuildScopeHistogramRenderData(counts, kBins);
  ASSERT_TRUE(data.valid);
  for (int channel = 0; channel < 3; ++channel) {
    EXPECT_FLOAT_EQ(data.rgb[Index(channel, 128)], 1.0f);
    for (int bin = 0; bin < kBins; ++bin) {
      EXPECT_EQ(data.peak_cut[Index(channel, bin)], 0U) << "bin " << bin;
    }
    // The order of the counts is kept.
    for (int bin = 1; bin <= 128; ++bin) {
      EXPECT_LE(data.rgb[Index(channel, bin - 1)], data.rgb[Index(channel, bin)]) << "bin " << bin;
    }
  }
}

TEST(ScopeHistogramRenderDataTest, ClippedHighlightTailDoesNotSetTheReference) {
  std::vector<uint32_t> counts(static_cast<size_t>(kBins) * 3U, 0U);
  for (int channel = 0; channel < 3; ++channel) {
    for (int bin = 40; bin < 200; ++bin) {
      counts[Index(channel, bin)] = 2000U;
    }
    counts[Index(channel, kBins - 1)] = 400000U;
  }
  const auto data = BuildScopeHistogramRenderData(counts, kBins);
  ASSERT_TRUE(data.valid);
  EXPECT_TRUE(data.highlight_clip_warning);
  EXPECT_FALSE(data.shadow_clip_warning);
  for (int channel = 0; channel < 3; ++channel) {
    EXPECT_FLOAT_EQ(data.rgb[Index(channel, 100)], 1.0f);
    EXPECT_EQ(data.peak_cut[Index(channel, 100)], 0U);
    EXPECT_EQ(data.peak_cut[Index(channel, kBins - 1)], 1U);
  }
}

TEST(ScopeHistogramRenderDataTest, ShortCountBufferIsInvalid) {
  const std::vector<uint32_t> counts(static_cast<size_t>(kBins) * 2U, 1U);
  EXPECT_FALSE(BuildScopeHistogramRenderData(counts, kBins).valid);
  EXPECT_FALSE(BuildScopeHistogramRenderData({}, 0).valid);
}

}  // namespace
}  // namespace alcedo
