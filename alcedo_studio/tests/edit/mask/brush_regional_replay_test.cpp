//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "brush_replay_oracle.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/mask/brush_canonical_sampler.hpp"
#include "edit/mask/brush_mask_commands.hpp"
#include "edit/mask/brush_rasterizer.hpp"
#include "edit/mask/brush_signed_distance.hpp"
#include "edit/mask/brush_source_geometry.hpp"
#include "edit/mask/brush_spatial_index.hpp"
#include "edit/mask/grade_mask_coverage.hpp"
#include "edit/mask/mask_model.hpp"
#include "grade_owned_mask_support.hpp"

#include <gtest/gtest.h>

#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace alcedo {
namespace {

constexpr Extent2D kRaster{96, 64};
constexpr Extent2D kFull{96, 64};
constexpr std::uint32_t kTile = 16;

void ExpectR8Equal(const std::vector<std::uint8_t>& actual,
                   const std::vector<std::uint8_t>& expected, Extent2D extent) {
  ASSERT_EQ(actual.size(), expected.size());
  for (std::uint32_t y = 0; y < extent.height; ++y) {
    for (std::uint32_t x = 0; x < extent.width; ++x) {
      const auto i = PackedR8Index(x, y, extent);
      ASSERT_EQ(actual[i], expected[i]) << "texel " << x << "," << y;
    }
  }
}

auto PaintStroke(std::string_view id, std::vector<BrushCanonicalSample> samples) -> BrushStroke {
  return MakeBrushStroke(StrokeId{std::string{id}}, BrushStrokeMode::Paint, std::move(samples));
}

auto EraseStroke(std::string_view id, std::vector<BrushCanonicalSample> samples) -> BrushStroke {
  return MakeBrushStroke(StrokeId{std::string{id}}, BrushStrokeMode::Erase, std::move(samples));
}

auto MakeBrush(std::vector<BrushStroke> strokes, Vector2 translation = {}, float feather = 0.0f)
    -> BrushMaskSource {
  BrushMaskSource source;
  source.strokes               = std::move(strokes);
  source.placement_translation = translation;
  source.feather_radius        = feather;
  return source;
}

auto MakeBrushMask(MaskId id, BrushMaskSource source, bool invert = false, float opacity = 1.0f)
    -> MaskModel {
  MaskModel mask;
  mask.id      = std::move(id);
  mask.invert  = invert;
  mask.opacity = opacity;
  mask.source  = std::move(source);
  return mask;
}

TEST(BrushRegionalReplay, RegionalBrushReplayMatchesFullEvaluation) {
  auto source = MakeBrush({
      PaintStroke("paint.a", {{12.0f, 10.0f, 6.0f, 1.0f, 1.0f},
                              {18.0f, 12.0f, 6.0f, 0.75f, 0.5f}}),
      EraseStroke("erase.b", {{16.0f, 11.0f, 4.0f, 1.0f, 1.0f}}),
      PaintStroke("paint.c", {{70.0f, 50.0f, 5.0f, 1.0f, 1.0f}}),
  });
  BrushSpatialIndex index(kTile);
  index.Rebuild(source, kRaster, kFull);
  BrushRasterizer rasterizer;
  rasterizer.SetGeometry(kRaster, kFull);
  const auto dirty = BrushSourceOutputTexelSupport(source, kRaster, kFull);
  rasterizer.ReplayRegion(source, index, dirty);
  ExpectR8Equal(std::vector<std::uint8_t>(rasterizer.Pixels().begin(), rasterizer.Pixels().end()),
                brush_replay_oracle::BrushSourceR8(source, kRaster, kFull), kRaster);

  const auto center = CanonicalBrushTexelReferenceCenter(12, 10, kRaster, kFull);
  EXPECT_NEAR(center.x, 12.5f, 1.0e-5f);
  EXPECT_NEAR(center.y, 10.5f, 1.0e-5f);
  const auto distant = PackedR8Index(70, 50, kRaster);
  EXPECT_EQ(rasterizer.Pixels()[distant], 255);

  BrushRasterizer full;
  full.SetGeometry(kRaster, kFull);
  full.RasterizeFull(source);
  ExpectR8Equal(std::vector<std::uint8_t>(full.Pixels().begin(), full.Pixels().end()),
                std::vector<std::uint8_t>(rasterizer.Pixels().begin(), rasterizer.Pixels().end()),
                kRaster);

  const auto before_move = BrushSourceOutputTexelSupport(source, kRaster, kFull);
  source.placement_translation = {8.0f, -4.0f};
  const auto after_move = BrushSourceOutputTexelSupport(source, kRaster, kFull);
  rasterizer.ReplayRegion(source, index, UnionTexelRect(before_move, after_move));
  ExpectR8Equal(std::vector<std::uint8_t>(rasterizer.Pixels().begin(), rasterizer.Pixels().end()),
                brush_replay_oracle::BrushSourceR8(source, kRaster, kFull), kRaster);
}

TEST(BrushRegionalReplay, EraseUndoRestoresEarlierPaint) {
  auto      grade = ColorGradeNodeModel::MakeClean(NodeId{"grade.primary"});
  MaskModel mask;
  mask.id     = MaskId{"mask.brush"};
  mask.source = BrushMaskSource{};
  grade->AddMask(std::move(mask), 0);
  const auto paint =
      PaintStroke("paint", {{20.0f, 16.0f, 8.0f, 1.0f, 1.0f}, {28.0f, 18.0f, 8.0f, 1.0f, 1.0f}});
  const auto erase = EraseStroke("erase", {{24.0f, 17.0f, 6.0f, 1.0f, 1.0f}});
  const auto id    = MaskId{"mask.brush"};
  grade->AppendBrushStroke({grade->Id(), id, paint, grade->MaskContentRevision(id)});
  BrushMaskSource painted = std::get<BrushMaskSource>(grade->FindMask(id)->source);
  GradeMaskCoverage coverage;
  coverage.SetGeometry(kRaster, kFull, kTile);
  coverage.BindBrushSource(id, painted);
  coverage.EvaluateFull(grade->Masks());
  const auto paint_pixels =
      std::vector<std::uint8_t>(coverage.Pixels().begin(), coverage.Pixels().end());

  grade->AppendBrushStroke({grade->Id(), id, erase, grade->MaskContentRevision(id)});
  auto erased = std::get<BrushMaskSource>(grade->FindMask(id)->source);
  coverage.BindBrushSource(id, erased);
  const auto erase_support = BrushSourceOutputTexelSupport(erased, kRaster, kFull);
  coverage.ReplayRegion(grade->Masks(), erase_support);
  ExpectR8Equal(std::vector<std::uint8_t>(coverage.Pixels().begin(), coverage.Pixels().end()),
                brush_replay_oracle::GradeMixR8(grade->Masks(), kRaster, kFull), kRaster);

  const auto erase_only_support = BrushStrokeOutputTexelSupport(
      erase, erased.placement_translation, kRaster, kFull);
  grade->RemoveBrushStroke({grade->Id(), id, StrokeId{"erase"}, grade->MaskContentRevision(id)});
  auto restored = std::get<BrushMaskSource>(grade->FindMask(id)->source);
  coverage.BindBrushSource(id, restored);
  coverage.ReplayRegion(grade->Masks(), erase_only_support);
  ExpectR8Equal(std::vector<std::uint8_t>(coverage.Pixels().begin(), coverage.Pixels().end()),
                paint_pixels, kRaster);
  ExpectR8Equal(paint_pixels, brush_replay_oracle::GradeMixR8(grade->Masks(), kRaster, kFull),
                kRaster);

  EXPECT_EQ(PaintBrushR8(30, 200), 200);
  EXPECT_EQ(PaintBrushR8(90, 200), 200);
  EXPECT_NE(30, 90);
}

TEST(BrushRegionalReplay, RemovingUnionMaximumPreservesOtherMasks) {
  RadialMaskSource radial;
  radial.center_x     = 0.35f;
  radial.center_y     = 0.40f;
  radial.major_radius = 0.45f;
  radial.minor_radius = 0.40f;
  auto radial_mask    = grade_mask_test::MakeRadialMask(MaskId{"mask.radial"}, radial);
  auto brush_source   = MakeBrush({PaintStroke("hot", {{32.0f, 24.0f, 10.0f, 1.0f, 1.0f}})});
  auto brush_mask     = MakeBrushMask(MaskId{"mask.brush"}, brush_source);
  std::vector<MaskModel> both{radial_mask, brush_mask};
  GradeMaskCoverage coverage;
  coverage.SetGeometry(kRaster, kFull, kTile);
  coverage.BindBrushSource(brush_mask.id, brush_source);
  coverage.EvaluateFull(both);
  ExpectR8Equal(std::vector<std::uint8_t>(coverage.Pixels().begin(), coverage.Pixels().end()),
                brush_replay_oracle::GradeMixR8(both, kRaster, kFull), kRaster);

  const auto brush_support = EffectiveMaskTexelSupport(brush_mask, kRaster, kFull);
  std::vector<MaskModel> remaining{radial_mask};
  coverage.UnbindMask(brush_mask.id);
  coverage.ReplayRegion(remaining, brush_support);
  const auto expected = brush_replay_oracle::GradeMixR8(remaining, kRaster, kFull);
  ExpectR8Equal(std::vector<std::uint8_t>(coverage.Pixels().begin(), coverage.Pixels().end()),
                expected, kRaster);

  const auto overlap = PackedR8Index(32, 24, kRaster);
  EXPECT_GT(expected[overlap], 0);
  EXPECT_EQ(std::vector<std::uint8_t>(coverage.Pixels().begin(), coverage.Pixels().end())[overlap],
            expected[overlap]);
  const auto unioned = brush_replay_oracle::GradeMixR8(both, kRaster, kFull);
  EXPECT_GE(unioned[overlap], expected[overlap]);
}

TEST(BrushRegionalReplay, BrushEventGroupingPreservesCanonicalPixels) {
  const Vector2 start{14.0f, 20.0f};
  const Vector2 end{54.0f, 20.0f};
  const float   radius = 8.0f;
  auto sample = [&](const std::vector<Vector2>& points) {
    BrushCanonicalSampler sampler;
    sampler.BeginStroke(BrushStrokeMode::Paint, points.front(), {}, radius, 1.0f, 0.5f);
    for (std::size_t i = 1; i < points.size(); ++i) {
      sampler.AppendReference(points[i]);
    }
    return sampler.FinishStroke();
  };
  std::vector<Vector2> sparse{start, end};
  std::vector<Vector2> grouped{start, {24.0f, 20.0f}, {40.0f, 20.0f}, end};
  std::vector<Vector2> dense{start};
  for (int i = 1; i <= 80; ++i) {
    dense.push_back({start.x + static_cast<float>(i) * 0.5f, start.y});
  }
  const auto sparse_samples  = sample(sparse);
  const auto grouped_samples = sample(grouped);
  const auto dense_samples   = sample(dense);
  EXPECT_EQ(sparse_samples, grouped_samples);
  EXPECT_EQ(sparse_samples, dense_samples);

  auto source = MakeBrush({PaintStroke("grouped", sparse_samples)});
  BrushSpatialIndex index(kTile);
  index.Rebuild(source, kRaster, kFull);
  BrushRasterizer rasterizer;
  rasterizer.SetGeometry(kRaster, kFull);
  rasterizer.ReplayRegion(source, index, BrushSourceOutputTexelSupport(source, kRaster, kFull));
  ExpectR8Equal(std::vector<std::uint8_t>(rasterizer.Pixels().begin(), rasterizer.Pixels().end()),
                brush_replay_oracle::BrushSourceR8(source, kRaster, kFull), kRaster);
}

TEST(BrushRegionalReplay, FeatherRebuildMatchesCompleteDistanceEvaluation) {
  auto source = MakeBrush({PaintStroke("soft", {{22.0f, 18.0f, 7.0f, 1.0f, 0.35f},
                                               {30.0f, 20.0f, 7.0f, 1.0f, 0.35f}})},
                          {}, 4.0f);
  auto mask   = MakeBrushMask(MaskId{"mask.brush"}, source);
  GradeMaskCoverage coverage;
  coverage.SetGeometry(kRaster, kFull, kTile);
  coverage.BindBrushSource(mask.id, source);
  const auto source_dirty = BrushSourceOutputTexelSupport(source, kRaster, kFull);
  EXPECT_FALSE(RectIEmpty(source_dirty));
  EXPECT_LT(source_dirty.width * source_dirty.height,
            static_cast<int>(kRaster.width * kRaster.height));
  coverage.ReplayRegion(std::span<const MaskModel>(&mask, 1), source_dirty);
  ExpectR8Equal(std::vector<std::uint8_t>(coverage.Pixels().begin(), coverage.Pixels().end()),
                brush_replay_oracle::GradeMixR8(std::span<const MaskModel>(&mask, 1), kRaster,
                                                kFull),
                kRaster);

  BrushRasterizer rasterizer;
  rasterizer.SetGeometry(kRaster, kFull);
  BrushSpatialIndex index(kTile);
  index.Rebuild(source, kRaster, kFull);
  rasterizer.ReplayRegion(source, index, source_dirty);
  const auto radius_texels = BrushFeatherRadiusToSourceTexels(
      source.feather_radius, kRaster, CanonicalBrushReferenceBounds(), kFull);
  BrushSignedDistanceFeather feather;
  const auto dt = feather.Apply(rasterizer.Pixels(), kRaster, radius_texels, false, 1.0f);
  const auto exact = brush_replay_oracle::ExactSignedDistanceFeather(
      brush_replay_oracle::BrushSourceR8(source, kRaster, kFull), kRaster, radius_texels, false,
      1.0f);
  ExpectR8Equal(dt, exact, kRaster);
}

TEST(BrushRegionalReplay, EmptyAndDisabledListsUseDefinedCoverageBase) {
  GradeMaskCoverage coverage;
  coverage.SetGeometry(kRaster, kFull, kTile);
  coverage.EvaluateFull({});
  auto all_one = std::vector<std::uint8_t>(coverage.Pixels().begin(), coverage.Pixels().end());
  EXPECT_EQ(all_one, brush_replay_oracle::GradeMixR8({}, kRaster, kFull));
  EXPECT_EQ(all_one.front(), 255);

  auto disabled = MakeBrushMask(MaskId{"mask.brush"},
                                MakeBrush({PaintStroke("p", {{8.0f, 8.0f, 4.0f, 1.0f, 1.0f}})}));
  disabled.enabled = false;
  coverage.BindBrushSource(disabled.id, std::get<BrushMaskSource>(disabled.source));
  coverage.EvaluateFull(std::span<const MaskModel>(&disabled, 1));
  auto all_zero = std::vector<std::uint8_t>(coverage.Pixels().begin(), coverage.Pixels().end());
  EXPECT_EQ(all_zero, brush_replay_oracle::GradeMixR8(std::span<const MaskModel>(&disabled, 1),
                                                     kRaster, kFull));
  EXPECT_EQ(all_zero.front(), 0);
}

TEST(BrushRegionalReplay, MissingBrushIndexAndUnsupportedAlgorithmFailWithoutPartialMix) {
  GradeMaskCoverage coverage;
  coverage.SetGeometry(kRaster, kFull, kTile);
  coverage.EvaluateFull({});
  auto before = std::vector<std::uint8_t>(coverage.Pixels().begin(), coverage.Pixels().end());
  auto mask   = MakeBrushMask(
      MaskId{"mask.brush"}, MakeBrush({PaintStroke("p", {{8.0f, 8.0f, 4.0f, 1.0f, 1.0f}})}));
  EXPECT_THROW(coverage.EvaluateFull(std::span<const MaskModel>(&mask, 1)), std::runtime_error);
  ExpectR8Equal(std::vector<std::uint8_t>(coverage.Pixels().begin(), coverage.Pixels().end()),
                before, kRaster);
  auto source = std::get<BrushMaskSource>(mask.source);
  source.raster_algorithm_version = 99;
  BrushSpatialIndex index(kTile);
  EXPECT_THROW(index.Rebuild(source, kRaster, kFull), std::runtime_error);
}

}  // namespace
}  // namespace alcedo
