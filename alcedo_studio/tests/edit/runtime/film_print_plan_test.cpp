//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/film_print_plan.hpp"

#include <gtest/gtest.h>

#include <array>
#include <span>
#include <stdexcept>
#include <string>

#include "edit/runtime/diffusion_filter_plan.hpp"

namespace alcedo {
namespace {

constexpr std::array<double, 3> kHalationStrength = {0.03, 0.008, 0.0};
constexpr std::array<double, 4> kSpreadGaussians  = {0.2528, 0.4142, 0.7472, 1.4271};
constexpr std::array<double, 3> kLayerSpreadUm    = {4.95, 3.02, 2.83};
constexpr std::array<double, 3> kNoSpread         = {0.0, 0.0, 0.0};

auto                            Constants() -> FilmImagePlaneConstants {
  return {.halation_strength   = kHalationStrength,
                                     .spread_gaussians    = kSpreadGaussians,
                                     .frame_mm            = 36.0,
                                     .halation_um         = 120.0,
                                     .inhibition_um       = 35.0,
                                     .enlarger_flare_size = 0.1};
}

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

auto FullFrame(std::uint32_t width, std::uint32_t height) -> ResolvedRenderGeometry {
  return View({width, height}, {width, height}, Matrix3x3::Identity());
}

/// The sum of a full symmetric kernel of one layer.
auto KernelSum(const FilmBlurKernel& kernel, std::size_t layer) -> double {
  double sum = kernel.weights[layer][0];
  for (std::uint32_t tap = 1; tap <= kernel.radius; ++tap) {
    sum += 2.0 * kernel.weights[layer][tap];
  }
  return sum;
}

TEST(FilmPrintImagePlan, A24MegapixelFrameHasTheSizesOfTheLab) {
  const auto plan = MakeFilmPrintImagePlan(FullFrame(6000, 4000), Constants(), kLayerSpreadUm, true,
                                           FilmPrintParams{});
  ASSERT_TRUE(plan.image_plane);

  // Spread at 6 um per pixel: sigma 1.18 pixels for the wide red Gaussian, radius int(4 s + 0.5).
  EXPECT_EQ(plan.spread_passes, 2U);
  EXPECT_EQ(plan.spread_x[1].radius, 5U);
  EXPECT_FLOAT_EQ(plan.spread_weight[0] + plan.spread_weight[1], 1.0f);

  // The inhibitor: sigma 5.83 pixels, on a half-size image with sigma sqrt(2.92^2 - 1/4) = 2.87.
  EXPECT_TRUE(plan.inhibitor_half_size);
  EXPECT_EQ(plan.inhibitor_extent.width, 3000U);
  EXPECT_EQ(plan.inhibitor_extent.height, 2000U);
  EXPECT_EQ(plan.inhibitor_x.radius, 11U);

  // Halation: sigma 20 pixels of the frame, on the canvas sqrt((20 * 2048 / 6000)^2 - 1/4) = 6.81.
  EXPECT_EQ(plan.canvas_extent.width, static_cast<std::uint32_t>(kDiffusionCanvasMaxLongEdge));
  EXPECT_TRUE(plan.halation_active);
  EXPECT_EQ(plan.halation_x.radius, 27U);
  EXPECT_FLOAT_EQ(plan.halation_strength[0], 0.03f);
  EXPECT_FLOAT_EQ(plan.halation_strength[2], 0.0f);

  // Flare: sigma 204.8 canvas texels, so a factor of 34 gives about 6 texels on the small image.
  EXPECT_TRUE(plan.flare_active);
  EXPECT_FLOAT_EQ(plan.flare, 0.01f);
  EXPECT_EQ(plan.flare_extent.width, 60U);
  EXPECT_EQ(plan.flare_extent.height, 40U);
  EXPECT_EQ(plan.flare_x.radius, 24U);
}

TEST(FilmPrintImagePlan, TheNarrowSpreadFusesOnlyWhileItsRadiusFitsOneDispatch) {
  const auto plan = MakeFilmPrintImagePlan(FullFrame(6000, 4000), Constants(), kLayerSpreadUm, true,
                                           FilmPrintParams{});
  EXPECT_TRUE(plan.spread_narrow_fused);
  EXPECT_EQ(plan.spread_narrow.radius, 1U);
  EXPECT_EQ(plan.spread_narrow.x[0][1], plan.spread_x[0].weights[0][1]);
  EXPECT_EQ(plan.spread_narrow.y[2][0], plan.spread_y[0].weights[2][0]);

  FilmPrintParams wide;
  wide.light_spread = 3.0f;
  const auto wide_plan =
      MakeFilmPrintImagePlan(FullFrame(6000, 4000), Constants(), kLayerSpreadUm, true, wide);
  EXPECT_FALSE(wide_plan.spread_narrow_fused);
  EXPECT_GT(wide_plan.spread_x[0].radius, kFilmNarrowSpreadMaxRadius);
}

TEST(FilmPrintImagePlan, EachKernelHasASumOf1) {
  const auto plan = MakeFilmPrintImagePlan(FullFrame(6000, 4000), Constants(), kLayerSpreadUm, true,
                                           FilmPrintParams{});
  for (const auto* kernel : {&plan.spread_x[0], &plan.spread_y[0], &plan.spread_x[1],
                             &plan.spread_y[1], &plan.inhibitor_x, &plan.inhibitor_y,
                             &plan.halation_x, &plan.halation_y, &plan.flare_x, &plan.flare_y}) {
    for (std::size_t layer = 0; layer < 3; ++layer) {
      EXPECT_NEAR(KernelSum(*kernel, layer), 1.0, 1.0e-5);
    }
  }
}

TEST(FilmPrintImagePlan, RenderScaleSetsTheSizesOfTheRenderAndNotOfTheCanvas) {
  const auto full = MakeFilmPrintImagePlan(FullFrame(6000, 4000), Constants(), kLayerSpreadUm, true,
                                           FilmPrintParams{});
  const auto preview =
      MakeFilmPrintImagePlan(View({6000, 4000}, {1500, 1000}, Matrix3x3::Scale(4.0f, 4.0f)),
                             Constants(), kLayerSpreadUm, true, FilmPrintParams{});
  // Sigma 5.83 / 4 = 1.46 pixels stays at the size of the render.
  EXPECT_FALSE(preview.inhibitor_half_size);
  EXPECT_EQ(preview.inhibitor_extent.width, 1500U);
  EXPECT_LT(preview.spread_x[1].radius, full.spread_x[1].radius);

  EXPECT_EQ(preview.canvas_extent.width, full.canvas_extent.width);
  EXPECT_EQ(preview.halation_x.radius, full.halation_x.radius);
  for (std::uint32_t tap = 0; tap <= full.halation_x.radius; ++tap) {
    EXPECT_EQ(preview.halation_x.weights[0][tap], full.halation_x.weights[0][tap]);
  }
  EXPECT_EQ(preview.flare_extent.width, full.flare_extent.width);
  EXPECT_EQ(preview.flare_extent.height, full.flare_extent.height);
}

TEST(FilmPrintImagePlan, ARoiReadsTheCanvasAtItsReferencePosition) {
  const auto plan =
      MakeFilmPrintImagePlan(View({6000, 4000}, {800, 600}, Matrix3x3::Translate(3000.0f, 1000.0f)),
                             Constants(), kLayerSpreadUm, true, FilmPrintParams{});
  const auto canvas = TransformPoint(plan.render_to_canvas, {0.0f, 0.0f});
  EXPECT_NEAR(canvas.x, 3000.0f * plan.canvas_extent.width / 6000.0f, 1.0e-2f);
  EXPECT_NEAR(canvas.y, 1000.0f * plan.canvas_extent.height / 4000.0f, 1.0e-2f);
  const auto flare = TransformPoint(plan.render_to_flare, {0.0f, 0.0f});
  EXPECT_NEAR(flare.x, 3000.0f * plan.flare_extent.width / 6000.0f, 1.0e-2f);
  EXPECT_NEAR(flare.y, 1000.0f * plan.flare_extent.height / 4000.0f, 1.0e-2f);
}

TEST(FilmPrintImagePlan, ASmallFrameHasNoImagePlane) {
  const auto plan = MakeFilmPrintImagePlan(FullFrame(241, 1), Constants(), kLayerSpreadUm, true,
                                           FilmPrintParams{});
  EXPECT_FALSE(plan.image_plane);
  EXPECT_EQ(plan.spread_passes, 0U);
  EXPECT_FALSE(plan.halation_active);
  EXPECT_FALSE(plan.flare_active);
  EXPECT_TRUE(MakeFilmPrintImagePlan(FullFrame(16, 16), Constants(), kLayerSpreadUm, true,
                                     FilmPrintParams{})
                  .image_plane);
}

TEST(FilmPrintImagePlan, SmallCanvasKeepsTheFlareOnTheCanvas) {
  const auto plan = MakeFilmPrintImagePlan(FullFrame(100, 80), Constants(), kLayerSpreadUm, true,
                                           FilmPrintParams{});
  EXPECT_EQ(plan.canvas_extent.width, 100U);
  EXPECT_EQ(plan.flare_extent.width, 100U);
  EXPECT_EQ(plan.flare_extent.height, 80U);
  // Sigma 10 texels, radius int(40.5), with no reduction to take out.
  EXPECT_EQ(plan.flare_x.radius, 40U);
}

TEST(FilmPrintImagePlan, SlidersAtZeroTurnOffTheirSteps) {
  FilmPrintParams off;
  off.light_spread   = 0.0f;
  off.halation       = 0.0f;
  off.enlarger_flare = 0.0f;
  const auto plan =
      MakeFilmPrintImagePlan(FullFrame(6000, 4000), Constants(), kLayerSpreadUm, true, off);
  EXPECT_EQ(plan.spread_passes, 0U);
  EXPECT_FALSE(plan.halation_active);
  EXPECT_FALSE(plan.flare_active);
  EXPECT_TRUE(plan.inhibitor_half_size) << "the inhibition has no slider";

  EXPECT_EQ(
      MakeFilmPrintImagePlan(FullFrame(6000, 4000), Constants(), kNoSpread, true, FilmPrintParams{})
          .spread_passes,
      0U);
}

TEST(FilmPrintImagePlan, ALayerWithNoSpreadKeepsItsPixels) {
  const std::array<double, 3> spread = {4.95, 0.0, 2.83};
  const auto                  plan =
      MakeFilmPrintImagePlan(FullFrame(6000, 4000), Constants(), spread, true, FilmPrintParams{});
  EXPECT_EQ(plan.spread_x[1].weights[1][0], 1.0f);
  EXPECT_EQ(plan.spread_x[1].weights[1][1], 0.0f);
  EXPECT_GT(plan.spread_x[1].weights[0][1], 0.0f);
}

TEST(FilmPrintImagePlan, ABlurBeyondTheTapLimitThrowsAndNamesTheSigma) {
  FilmPrintParams wide;
  wide.light_spread = 3.0f;
  EXPECT_NO_THROW((void)MakeFilmPrintImagePlan(FullFrame(40000, 20000), Constants(), kLayerSpreadUm,
                                               true, wide));
  const std::array<double, 3> long_spread = {60.0, 60.0, 60.0};
  try {
    (void)MakeFilmPrintImagePlan(FullFrame(40000, 20000), Constants(), long_spread, true, wide);
    FAIL() << "the plan has no substitute blur";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("sigma"), std::string::npos) << error.what();
  }
}

}  // namespace
}  // namespace alcedo
