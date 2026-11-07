//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Host tests of the grain in the negative of the film print (plan
// 'film_grain_format_and_stock_plan.md', phase G5): the apply parameters of the plan and the apply
// step of film_grain_negative.h.

#include "edit/runtime/film/film_grain_negative.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <stdexcept>
#include <vector>

#include "edit/geometry/render_geometry_resolver.hpp"
#include "edit/geometry/source_geometry.hpp"
#include "edit/runtime/film/film_grain_field.h"
#include "edit/runtime/film/film_stock_bundle.hpp"
#include "edit/runtime/film_grain_plan.hpp"

namespace alcedo {
namespace {

/// Grain data with the constants of the shipped bundle (lab step P15). The layer curves have no
/// physical meaning; each has 4 nodes.
struct NegativeStockValues {
  std::array<double, 3>       cloud_sigma_um{2.0, 2.0, 2.8};
  std::array<double, 3>       layer_correlation{0.489, 0.2349, 0.1743};
  std::array<double, 3>       post_value{0.188, 0.191, 0.172};
  std::array<double, 3>       post_sigma_48{0.0107, 0.0100, 0.0152};
  std::array<double, 3>       post_correlation{0.535, 0.509, 0.087};
  std::vector<double>         x{0.2, 0.8, 1.4, 2.4, 0.3, 0.9, 1.5, 2.5, 1.0, 1.3, 2.0, 3.0};
  std::vector<double>         sigma{0.0056, 0.0108, 0.0084, 0.0048, 0.0052, 0.0108,
                            0.0082, 0.0062, 0.0118, 0.0158, 0.0110, 0.0042};
  std::array<std::int32_t, 3> count{4, 4, 4};
  bool                        reversal = false;

  [[nodiscard]] auto          View() const -> FilmGrainStockView {
    return {
                 .layer    = {x, sigma, count, 4},
                 .channel  = {x, sigma, count, 4},
                 .common   = {.cloud_sigma_um           = cloud_sigma_um,
                              .layer_correlation        = layer_correlation,
                              .post_display_value       = post_value,
                              .post_display_sigma_48    = post_sigma_48,
                              .post_display_correlation = post_correlation,
                              .aperture_area_um2        = std::numbers::pi * 24.0 * 24.0},
                 .tier     = reversal ? "reversal" : "curve",
                 .reversal = reversal,
    };
  }
};

auto Frame(std::uint32_t long_side) -> ResolvedRenderGeometry {
  const Extent2D extent{long_side, long_side * 2U / 3U};
  return ResolveRenderGeometry(MakeSourceGeometry(extent, extent), {}, {}, ResolutionRequest{}, {});
}

auto NegativePlan(bool reversal, float strength = 1.0f,
                  FilmGrainTarget target = FilmGrainTarget::Negative) -> FilmGrainPlan {
  NegativeStockValues stock;
  stock.reversal = reversal;
  return MakeFilmGrainPlan(Frame(6000), FilmFormat::Film16mm, stock.View(), target, strength);
}

constexpr std::array<double, 3> kDyeMax{2.6, 2.9, 3.1};

TEST(FilmGrainNegativePlan, ScaleIsTheStrengthTimesThePixelFactorOfEachLayer) {
  const auto plan   = NegativePlan(false, 1.5f);
  const auto params = MakeFilmGrainNegativeParams(plan, {});
  EXPECT_FALSE(plan.reversal);
  EXPECT_EQ(params.record_sign, 1.0f);
  for (std::size_t layer = 0; layer < 3; ++layer) {
    SCOPED_TRACE(layer);
    EXPECT_FLOAT_EQ(params.scale[layer], static_cast<float>(1.5 * plan.layers[layer].pixel_factor));
    EXPECT_EQ(params.dye_max[layer], 0.0f);
    EXPECT_EQ(params.curves[layer].last_segment, 2);
  }
}

// The node form of the apply step gives the segment form of the plan within the rounding, below the
// first node, on and between the nodes, and above the last node.
TEST(FilmGrainNegativePlan, NodeCurveEqualsTheSegmentCurveOfThePlan) {
  const auto plan   = NegativePlan(false);
  const auto params = MakeFilmGrainNegativeParams(plan, {});
  for (std::size_t layer = 0; layer < 3; ++layer) {
    SCOPED_TRACE(layer);
    for (float value = -0.5f; value <= 4.0f; value += 0.0125f) {
      const float segment = FilmGrainCurveSigma(&plan.curves[layer], value);
      EXPECT_NEAR(FilmGrainNodeCurveSigma(&params.curves[layer], value), segment, 1e-6f * segment)
          << value;
    }
    const auto count = plan.node_count[layer];
    for (std::uint32_t node = 0; node < count; ++node) {
      EXPECT_FLOAT_EQ(FilmGrainNodeCurveSigma(&params.curves[layer],
                                              static_cast<float>(plan.node_x[layer][node])),
                      static_cast<float>(plan.node_sigma[layer][node]));
    }
  }
}

// A curve of 16 nodes, the most of the bundle, reads its last segment and its end values.
TEST(FilmGrainNegativePlan, NodeCurveOfSixteenNodesReadsItsLastSegment) {
  NegativeStockValues stock;
  stock.x.clear();
  stock.sigma.clear();
  for (int layer = 0; layer < 3; ++layer) {
    for (int node = 0; node < 16; ++node) {
      stock.x.push_back(0.1 * node);
      stock.sigma.push_back(0.004 + 0.0005 * node + 0.001 * layer);
    }
  }
  stock.count = {16, 16, 16};
  auto view   = stock.View();
  view.layer  = {stock.x, stock.sigma, stock.count, 16};
  const auto plan =
      MakeFilmGrainPlan(Frame(6000), FilmFormat::Film35mm, view, FilmGrainTarget::Negative, 1.0f);
  const auto params = MakeFilmGrainNegativeParams(plan, {});
  for (std::size_t layer = 0; layer < 3; ++layer) {
    SCOPED_TRACE(layer);
    const float base = 0.004f + 0.001f * static_cast<float>(layer);
    EXPECT_EQ(params.curves[layer].last_segment, 14);
    EXPECT_NEAR(FilmGrainNodeCurveSigma(&params.curves[layer], 1.45f), base + 0.00725f, 1e-7f);
    EXPECT_NEAR(FilmGrainNodeCurveSigma(&params.curves[layer], 1.5f), base + 0.0075f, 1e-7f);
    EXPECT_NEAR(FilmGrainNodeCurveSigma(&params.curves[layer], 9.0f), base + 0.0075f, 1e-7f);
    EXPECT_NEAR(FilmGrainNodeCurveSigma(&params.curves[layer], -1.0f), base, 1e-7f);
  }
}

TEST(FilmGrainNegativePlan, ReversalFilmNeedsItsDyeMaxAndANegativeHasNone) {
  const auto reversal = NegativePlan(true);
  EXPECT_TRUE(reversal.reversal);
  const auto params = MakeFilmGrainNegativeParams(reversal, kDyeMax);
  EXPECT_EQ(params.record_sign, -1.0f);
  for (std::size_t layer = 0; layer < 3; ++layer) {
    EXPECT_FLOAT_EQ(params.dye_max[layer], static_cast<float>(kDyeMax[layer]));
  }
  EXPECT_THROW((void)MakeFilmGrainNegativeParams(reversal, {}), std::invalid_argument);
  const std::array<double, 3> not_finite{2.6, std::nan(""), 3.1};
  EXPECT_THROW((void)MakeFilmGrainNegativeParams(reversal, not_finite), std::invalid_argument);
  EXPECT_THROW((void)MakeFilmGrainNegativeParams(NegativePlan(false), kDyeMax),
               std::invalid_argument);
}

TEST(FilmGrainNegativePlan, PostPlanIsRejected) {
  EXPECT_THROW(
      (void)MakeFilmGrainNegativeParams(NegativePlan(false, 1.0f, FilmGrainTarget::Post), {}),
      std::invalid_argument);
}

TEST(FilmGrainNegativeApply, NegativeAddsTheScaledCurveSigmaOfTheAmount) {
  const auto                 plan   = NegativePlan(false, 0.8f);
  const auto                 params = MakeFilmGrainNegativeParams(plan, {});
  const FilmGrainNegativeRgb record{0.5f, 1.1f, 1.6f};
  const FilmGrainNoise3      unit{1.5f, -0.75f, 2.0f};
  const auto                 result = FilmGrainNegativeRecord(&params, record, unit);
  const std::array<float, 3> amounts{record.r, record.g, record.b};
  const std::array<float, 3> units{unit.x, unit.y, unit.z};
  const std::array<float, 3> got{result.r, result.g, result.b};
  for (std::size_t layer = 0; layer < 3; ++layer) {
    SCOPED_TRACE(layer);
    const float sigma = FilmGrainNodeCurveSigma(&params.curves[layer], amounts[layer]);
    EXPECT_FLOAT_EQ(got[layer], amounts[layer] + params.scale[layer] * sigma * units[layer]);
  }
  // No grain leaves the record unchanged.
  const auto still = FilmGrainNegativeRecord(&params, record, {0.0f, 0.0f, 0.0f});
  EXPECT_EQ(still.r, record.r);
  EXPECT_EQ(still.g, record.g);
  EXPECT_EQ(still.b, record.b);
}

// Lab rule 'removed_dye_record': the record of a reversal film is the removed dye, so the dye
// amount dye_max - record gets the change, with the curve read at that amount.
TEST(FilmGrainNegativeApply, ReversalFilmChangesTheDyeAmountDyeMaxMinusRecord) {
  const auto                 plan   = NegativePlan(true, 1.0f);
  const auto                 params = MakeFilmGrainNegativeParams(plan, kDyeMax);
  const FilmGrainNegativeRgb record{2.0f, 1.2f, 0.4f};
  const FilmGrainNoise3      unit{0.9f, 1.7f, -1.1f};
  const auto                 result = FilmGrainNegativeRecord(&params, record, unit);
  const std::array<float, 3> records{record.r, record.g, record.b};
  const std::array<float, 3> units{unit.x, unit.y, unit.z};
  const std::array<float, 3> got{result.r, result.g, result.b};
  for (std::size_t layer = 0; layer < 3; ++layer) {
    SCOPED_TRACE(layer);
    const float amount       = static_cast<float>(kDyeMax[layer]) - records[layer];
    const float sigma        = FilmGrainNodeCurveSigma(&params.curves[layer], amount);
    const float dye_after    = static_cast<float>(kDyeMax[layer]) - got[layer];
    const float dye_expected = amount + params.scale[layer] * sigma * units[layer];
    EXPECT_NEAR(dye_after, dye_expected, 1e-6f);
  }
}

}  // namespace
}  // namespace alcedo
