//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/adjustment_runtime.hpp"

#include <gtest/gtest.h>

#include "edit/geometry/render_geometry_resolver.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/operators/models/sharpen_model.hpp"

namespace alcedo {
namespace {

auto MakeGeometry(std::uint32_t extent, const ResolutionRequest& resolution = {},
                  const ViewRequest& view = {}) -> ResolvedRenderGeometry {
  return ResolveRenderGeometry(MakeSourceGeometry({extent, extent}, {extent, extent}), {}, view,
                               resolution, {});
}

}  // namespace

TEST(GpuDagAdjustmentRuntime, SharpenAndClarityRadiiScaleWithPreviewResolution) {
  SharpenModel sharpen;
  sharpen.SetAmount(100.0f);
  sharpen.SetRadius(8.0f);
  ClarityModel clarity;
  clarity.SetValue(80.0f);

  const auto full = MakeGeometry(256);
  const auto full_sharpen =
      MakeGradeNeighborParams(sharpen, AdjustmentBehavior::Sharpen, full);
  const auto full_clarity =
      MakeGradeNeighborParams(clarity, AdjustmentBehavior::Clarity, full);

  ResolutionRequest half;
  half.render_scale = 0.5f;
  const auto half_geometry = MakeGeometry(256, half);
  const auto half_sharpen =
      MakeGradeNeighborParams(sharpen, AdjustmentBehavior::Sharpen, half_geometry);
  const auto half_clarity =
      MakeGradeNeighborParams(clarity, AdjustmentBehavior::Clarity, half_geometry);

  EXPECT_EQ(full.render_extent, (Extent2D{256, 256}));
  EXPECT_EQ(half_geometry.render_extent, (Extent2D{128, 128}));
  EXPECT_NEAR(full_sharpen.sigma_x, 8.0f, 1.0e-5f);
  EXPECT_NEAR(half_sharpen.sigma_x, 4.0f, 1.0e-4f);
  EXPECT_EQ(full_sharpen.radius, 15U);
  EXPECT_EQ(half_sharpen.radius, 12U);
  EXPECT_NEAR(half_clarity.sigma_x, full_clarity.sigma_x * 0.5f, 1.0e-4f);
  EXPECT_LT(half_clarity.radius, full_clarity.radius);
  EXPECT_GT(half_clarity.radius, 0U);
}

TEST(GpuDagAdjustmentRuntime, HalationAndFilmGrainNeighborhoodsScaleWithMaxEdge) {
  HalationModel halation;
  halation.SetValue(1.0f);
  FilmGrainModel grain;
  grain.SetValue(0.75f);

  const auto full = MakeGeometry(256);
  const auto full_halation =
      MakeGradeNeighborParams(halation, AdjustmentBehavior::Halation, full);
  const auto full_grain =
      MakeGradeNeighborParams(grain, AdjustmentBehavior::FilmGrain, full);

  ResolutionRequest preview;
  preview.max_edge = 64;
  const auto preview_geometry = MakeGeometry(256, preview);
  const auto preview_halation =
      MakeGradeNeighborParams(halation, AdjustmentBehavior::Halation, preview_geometry);
  const auto preview_grain =
      MakeGradeNeighborParams(grain, AdjustmentBehavior::FilmGrain, preview_geometry);

  EXPECT_EQ(preview_geometry.render_extent, (Extent2D{64, 64}));
  EXPECT_NEAR(full_halation.sigma_x, 7.0f, 1.0e-5f);
  EXPECT_NEAR(preview_halation.sigma_x, 1.75f, 1.0e-4f);
  EXPECT_NEAR(preview_halation.sigma_y, preview_halation.sigma_x, 1.0e-5f);
  EXPECT_NEAR(preview_grain.sigma_x, full_grain.sigma_x * 0.25f, 1.0e-4f);
  EXPECT_LT(preview_grain.radius, full_grain.radius);
  EXPECT_EQ(full_grain.radius, 3U);
  EXPECT_EQ(preview_grain.tap_count, preview_grain.radius + 1U);
}

TEST(GpuDagAdjustmentRuntime, NativeViewportRoiKeepsFullReferenceNeighborhoodSize) {
  SharpenModel sharpen;
  sharpen.SetAmount(50.0f);
  sharpen.SetRadius(6.0f);

  const auto full = MakeGradeNeighborParams(sharpen, AdjustmentBehavior::Sharpen, MakeGeometry(256));

  ViewRequest view;
  view.visible_rect_in_edit_space = {0.25f, 0.25f, 0.5f, 0.5f};
  view.viewport_extent            = {128, 128};
  const auto roi =
      MakeGradeNeighborParams(sharpen, AdjustmentBehavior::Sharpen, MakeGeometry(256, {}, view));

  EXPECT_NEAR(roi.sigma_x, full.sigma_x, 1.0e-4f);
  EXPECT_EQ(roi.radius, full.radius);
}

}  // namespace alcedo
