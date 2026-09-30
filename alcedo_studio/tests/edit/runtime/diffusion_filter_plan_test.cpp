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

/// Geometry of a render of @p render pixels placed in @p reference by @p render_to_reference.
auto View(Extent2D reference, Extent2D render, const Matrix3x3& render_to_reference)
    -> ResolvedRenderGeometry {
  ResolvedRenderGeometry geometry;
  geometry.full_reference_extent = reference;
  geometry.edit_extent           = reference;
  geometry.render_extent         = render;
  geometry.render_to_reference   = render_to_reference;
  geometry.reference_to_render   = InvertAffine(render_to_reference);
  geometry.edit_to_render        = geometry.reference_to_render;
  return geometry;
}

TEST(DiffusionFilterPlan, CanvasIsTheReferenceFrameCappedAtTheLlfLongEdge) {
  const auto large = DiffusionCanvasExtent({6000, 4000});
  EXPECT_EQ(large.width, static_cast<std::uint32_t>(kDiffusionCanvasMaxLongEdge));
  EXPECT_EQ(large.height, 1366U);
  const auto small = DiffusionCanvasExtent({1200, 800});
  EXPECT_EQ(small.width, 1200U);
  EXPECT_EQ(small.height, 800U);
}

TEST(DiffusionFilterPlan, CappedCanvasLayoutUsesExpectedBaseLevelAndLevelCount) {
  // Canvas 2048 x 1366: sigma_base = 2.73 texels (level 1), sigma_max = 164 texels -> 7 levels.
  const auto layout = MakeDiffusionFilterLayout(DiffusionCanvasExtent({6000, 4000}),
                                                ResolveDiffusionFilterShape(0.5f));
  EXPECT_EQ(layout.base_level, 1U);
  EXPECT_EQ(layout.level_count, 7U);
  EXPECT_EQ(layout.extents[0].width, 1024U);
  EXPECT_EQ(layout.extents[0].height, 683U);
  EXPECT_EQ(layout.extents[6].width, 16U);
  EXPECT_EQ(layout.extents[6].height, 11U);
}

TEST(DiffusionFilterPlan, WeightsSumToOneAndFollowThePowerLawWithoutBlackMist) {
  auto shape        = ResolveDiffusionFilterShape(1.0f);
  shape.black_mist  = 0.0f;
  const auto layout = MakeDiffusionFilterLayout({2000, 1500}, shape);
  EXPECT_NEAR(WeightSum(layout), 1.0, 1e-6);
  ASSERT_GE(layout.level_count, 3U);
  // w_k is proportional to sigma_k^(2 - p) and sigma doubles per level.
  const double ratio = std::pow(2.0, 2.0 - shape.power_law_exponent);
  EXPECT_NEAR(layout.weights[1] / layout.weights[0], ratio, 1e-5);
  EXPECT_NEAR(layout.weights[2] / layout.weights[1], ratio, 1e-5);
}

TEST(DiffusionFilterPlan, BlackMistReducesTheWidestLevelAndTransmission) {
  auto clear        = ResolveDiffusionFilterShape(1.0f);
  clear.black_mist  = 0.0f;
  auto misted       = clear;
  misted.black_mist = 1.0f;
  const auto clear_layout  = MakeDiffusionFilterLayout({2000, 1500}, clear);
  const auto misted_layout = MakeDiffusionFilterLayout({2000, 1500}, misted);
  ASSERT_EQ(clear_layout.level_count, misted_layout.level_count);
  const auto last = misted_layout.level_count - 1;
  EXPECT_EQ(misted_layout.weights[last], 0.0f);
  EXPECT_GT(clear_layout.weights[last], 0.0f);
  EXPECT_NEAR(WeightSum(misted_layout), 1.0, 1e-6);
  EXPECT_EQ(clear_layout.transmission, 1.0f);
  EXPECT_LT(misted_layout.transmission, 1.0f);
}

TEST(DiffusionFilterPlan, TinyCanvasKeepsOneFullWeightBaseLevel) {
  // Both sigmas clamp to one texel, so the scatter image is the boosted canvas itself.
  const auto layout = MakeDiffusionFilterLayout({3, 2}, ResolveDiffusionFilterShape(1.0f));
  ASSERT_EQ(layout.level_count, 1U);
  EXPECT_EQ(layout.base_level, 0U);
  EXPECT_EQ(layout.extents[0].width, 3U);
  EXPECT_EQ(layout.extents[0].height, 2U);
  EXPECT_FLOAT_EQ(layout.weights[0], 1.0f);
}

TEST(DiffusionFilterPlan, EmptyCanvasIsRejected) {
  const auto shape = ResolveDiffusionFilterShape(0.5f);
  EXPECT_THROW((void)MakeDiffusionFilterLayout({0, 10}, shape), std::invalid_argument);
}

TEST(DiffusionFilterPlan, ReduceSamplesFollowTheRenderResolution) {
  const Extent2D reference{6000, 4000};
  const auto     layout = MakeDiffusionFilterLayout(DiffusionCanvasExtent(reference),
                                                    ResolveDiffusionFilterShape(0.5f));
  // One base texel spans 6000 / 1024 = 5.9 reference pixels.
  const auto full = MakeDiffusionScatterMapping(View(reference, reference, {}), layout);
  EXPECT_EQ(full.reduce_samples, 6U);
  const auto preview =
      MakeDiffusionScatterMapping(View(reference, {1500, 1000}, Matrix3x3::Scale(4.0f, 4.0f)),
                                  layout);
  EXPECT_EQ(preview.reduce_samples, 2U);
  const auto tiny =
      MakeDiffusionScatterMapping(View(reference, {300, 200}, Matrix3x3::Scale(20.0f, 20.0f)),
                                  layout);
  EXPECT_EQ(tiny.reduce_samples, 1U);
}

TEST(DiffusionFilterPlan, RoiAndPreviewPixelsMapToTheSameScatterPosition) {
  const Extent2D reference{6000, 4000};
  const auto     layout = MakeDiffusionFilterLayout(DiffusionCanvasExtent(reference),
                                                    ResolveDiffusionFilterShape(0.5f));
  const auto full = MakeDiffusionScatterMapping(View(reference, reference, {}), layout);
  // A 2x-zoomed ROI whose top-left render pixel starts at reference (1000, 700).
  const auto roi_to_reference =
      Matrix3x3::Translate(1000.0f, 700.0f) * Matrix3x3::Scale(0.5f, 0.5f);
  const auto roi = MakeDiffusionScatterMapping(View(reference, {800, 600}, roi_to_reference),
                                               layout);
  const auto roi_base  = TransformPoint(roi.render_to_base, {41.0f, 23.0f});
  const auto full_base = TransformPoint(full.render_to_base, {1020.5f, 711.5f});
  EXPECT_NEAR(roi_base.x, full_base.x, 1e-3f);
  EXPECT_NEAR(roi_base.y, full_base.y, 1e-3f);
  // The zoomed ROI averages at least as many samples per texel as the full render.
  EXPECT_GE(roi.reduce_samples, full.reduce_samples);
}

TEST(DiffusionFilterPlan, FullEditRenderPublishesAndRoiRenderSamples) {
  const auto full = DecideDiffusionScatter(true, false, true, 2560);
  EXPECT_EQ(full.action, DiffusionScatterAction::Rebuild);
  EXPECT_TRUE(full.persist_canonical);
  EXPECT_EQ(full.required_detail, 2560U);

  const auto roi_hit = DecideDiffusionScatter(true, true, false, 3000);
  EXPECT_EQ(roi_hit.action, DiffusionScatterAction::SampleCanonical);
  EXPECT_EQ(roi_hit.required_detail, 0U);

  const auto roi_miss = DecideDiffusionScatter(true, false, false, 3000);
  EXPECT_EQ(roi_miss.action, DiffusionScatterAction::Rebuild);
  EXPECT_FALSE(roi_miss.persist_canonical);

  const auto quality_base = DecideDiffusionScatter(false, false, true, 6000);
  EXPECT_EQ(quality_base.action, DiffusionScatterAction::Rebuild);
  EXPECT_FALSE(quality_base.persist_canonical);
}

}  // namespace
}  // namespace alcedo
