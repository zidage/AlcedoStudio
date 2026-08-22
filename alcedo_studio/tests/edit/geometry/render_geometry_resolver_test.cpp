//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "edit/geometry/render_geometry_resolver.hpp"

namespace alcedo {
namespace {

constexpr float kPxEps = 1e-4f;

auto ResolveSimple(const SourceGeometry& source, const ImageGeometryParams& image = {},
                   const ViewRequest& view = {}, const ResolutionRequest& resolution = {},
                   const SamplingFootprint& footprint = {}) -> ResolvedRenderGeometry {
  return ResolveRenderGeometry(source, image, view, resolution, footprint);
}

}  // namespace

TEST(GpuDagGeometry, RenderGeometryRoundTripsReferenceAndRenderPixelCenters) {
  ImageGeometryParams image;
  image.crop_rect         = NormalizedRect{0.10f, 0.15f, 0.70f, 0.60f};
  image.rotation_degrees  = 23.0f;
  image.expand_to_fit     = true;
  ViewRequest view;
  view.visible_rect_in_edit_space = NormalizedRect{0.20f, 0.10f, 0.50f, 0.60f};
  view.viewport_extent            = Extent2D{320, 240};
  const auto geometry =
      ResolveSimple(MakeSourceGeometry({80, 60}, {80, 60}), image, view, {}, {});

  float max_err = 0.0f;
  for (std::uint32_t y = 0; y < geometry.render_extent.height; y += 7) {
    for (std::uint32_t x = 0; x < geometry.render_extent.width; x += 11) {
      const auto center = PixelCenter(x, y);
      const auto ref    = TransformPoint(geometry.render_to_reference, center);
      const auto back   = TransformPoint(geometry.reference_to_render, ref);
      max_err           = std::max(max_err, std::fabs(back.x - center.x));
      max_err           = std::max(max_err, std::fabs(back.y - center.y));
    }
  }
  EXPECT_LT(max_err, kPxEps);
}

TEST(GpuDagGeometry, FullCropZeroRotationMapsReferenceCornersToRenderCorners) {
  const auto geometry = ResolveSimple(MakeSourceGeometry({64, 48}, {64, 48}));
  EXPECT_EQ(geometry.render_extent, (Extent2D{64, 48}));
  EXPECT_EQ(geometry.edit_extent, (Extent2D{64, 48}));
  EXPECT_TRUE(IsIdentityResample(geometry));

  const auto top_left = TransformPoint(geometry.render_to_reference, PixelCenter(0, 0));
  EXPECT_NEAR(top_left.x, 0.5f, kPxEps);
  EXPECT_NEAR(top_left.y, 0.5f, kPxEps);

  const auto bottom_right = TransformPoint(geometry.render_to_reference, PixelCenter(63, 47));
  EXPECT_NEAR(bottom_right.x, 63.5f, kPxEps);
  EXPECT_NEAR(bottom_right.y, 47.5f, kPxEps);
}

TEST(GpuDagGeometry, RotatedCropBoundsContainAllFourTransformedCorners) {
  ImageGeometryParams image;
  image.crop_rect        = NormalizedRect{0.25f, 0.25f, 0.50f, 0.50f};
  image.rotation_degrees = 35.0f;
  image.expand_to_fit    = true;
  const auto geometry = ResolveSimple(MakeSourceGeometry({200, 100}, {200, 100}), image);

  const Vector2 corners[] = {{50.0f, 25.0f}, {150.0f, 25.0f}, {150.0f, 75.0f}, {50.0f, 75.0f}};
  const float   edit_w    = static_cast<float>(geometry.edit_extent.width);
  const float   edit_h    = static_cast<float>(geometry.edit_extent.height);
  for (const auto& corner : corners) {
    const auto edit = TransformPoint(geometry.reference_to_edit, corner);
    EXPECT_GE(edit.x, -1.0e-3f);
    EXPECT_LE(edit.x, edit_w + 1.0e-3f);
    EXPECT_GE(edit.y, -1.0e-3f);
    EXPECT_LE(edit.y, edit_h + 1.0e-3f);
  }
  EXPECT_GT(geometry.edit_extent.width, 0u);
  EXPECT_GT(geometry.edit_extent.height, 0u);
}

TEST(GpuDagGeometry, ViewportCropAndDynamicScaleProduceRequestedRenderExtent) {
  ViewRequest view;
  view.viewport_extent = Extent2D{1920, 1080};
  ResolutionRequest half;
  half.render_scale = 0.5f;
  const auto scaled =
      ResolveSimple(MakeSourceGeometry({100, 100}, {100, 100}), {}, view, half, {});
  EXPECT_EQ(scaled.render_extent, (Extent2D{960, 540}));

  ResolutionRequest clamped;
  clamped.render_scale = 1.0f;
  clamped.max_edge     = 800;
  const auto capped =
      ResolveSimple(MakeSourceGeometry({100, 100}, {100, 100}), {}, view, clamped, {});
  EXPECT_EQ(capped.render_extent, (Extent2D{800, 450}));
}

TEST(GpuDagGeometry, DecodeScaleDoesNotChangeNormalizedReferenceCoordinates) {
  ImageGeometryParams image;
  image.crop_rect = NormalizedRect{0.25f, 0.25f, 0.50f, 0.50f};
  const auto full =
      ResolveSimple(MakeSourceGeometry({400, 300}, {400, 300}), image);
  const auto quarter =
      ResolveSimple(MakeSourceGeometry({100, 75}, {400, 300}, {}, 2), image);

  EXPECT_EQ(full.edit_extent, quarter.edit_extent);
  EXPECT_EQ(full.render_extent, quarter.render_extent);

  const auto n_full = NormalizedFromPixelCenter(
      TransformPoint(full.render_to_reference, PixelCenter(10, 8)), full.full_reference_extent);
  const auto n_quarter = NormalizedFromPixelCenter(
      TransformPoint(quarter.render_to_reference, PixelCenter(10, 8)),
      quarter.full_reference_extent);
  EXPECT_NEAR(n_full.x, n_quarter.x, 1.0e-5f);
  EXPECT_NEAR(n_full.y, n_quarter.y, 1.0e-5f);

  const auto n_decoded = NormalizedFromPixelCenter(PixelCenter(3, 5), Extent2D{100, 75});
  const auto n_ref     = NormalizedFromPixelCenter(
      TransformPoint(quarter.decoded_to_reference, PixelCenter(3, 5)), Extent2D{400, 300});
  EXPECT_NEAR(n_decoded.x, n_ref.x, 1.0e-5f);
  EXPECT_NEAR(n_decoded.y, n_ref.y, 1.0e-5f);
}

TEST(GpuDagGeometry, RequiredInputRegionExpandsForBicubicFootprintAndClampsToDecodedBounds) {
  ViewRequest view;
  view.visible_rect_in_edit_space = NormalizedRect{0.0f, 0.0f, 0.25f, 0.25f};
  SamplingFootprint footprint;
  footprint.radius_x = 2.0f;
  footprint.radius_y = 2.0f;
  const auto edge = ResolveSimple(MakeSourceGeometry({64, 64}, {64, 64}), {}, view, {}, footprint);
  EXPECT_EQ(edge.required_decoded_region.x, 0);
  EXPECT_EQ(edge.required_decoded_region.y, 0);
  EXPECT_GE(edge.required_decoded_region.width, 16);
  EXPECT_LE(edge.required_decoded_region.X1(), 64);
  EXPECT_LE(edge.required_decoded_region.Y1(), 64);

  view.visible_rect_in_edit_space = NormalizedRect{0.25f, 0.25f, 0.50f, 0.50f};
  const auto interior =
      ResolveSimple(MakeSourceGeometry({64, 64}, {64, 64}), {}, view, {}, footprint);
  EXPECT_GE(interior.required_decoded_region.x, 0);
  EXPECT_LT(interior.required_decoded_region.x, 16);
  EXPECT_GT(interior.required_decoded_region.X1(), 48);
  EXPECT_LE(interior.required_decoded_region.X1(), 64);
}

TEST(GpuDagGeometry, ResolveRenderGeometryRejectsZeroExtent) {
  EXPECT_THROW(ResolveSimple(MakeSourceGeometry({0, 10}, {10, 10})), std::runtime_error);
  EXPECT_THROW(ResolveSimple(MakeSourceGeometry({10, 10}, {0, 10})), std::runtime_error);
  ResolutionRequest bad;
  bad.render_scale = 0.0f;
  EXPECT_THROW(ResolveSimple(MakeSourceGeometry({10, 10}, {10, 10}), {}, {}, bad, {}),
               std::runtime_error);
}

}  // namespace alcedo
