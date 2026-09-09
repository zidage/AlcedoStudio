//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "brush_replay_oracle.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/mask/brush_canonical_sampler.hpp"
#include "edit/mask/brush_placement.hpp"
#include "edit/mask/brush_rasterizer.hpp"
#include "edit/mask/brush_source_geometry.hpp"
#include "edit/mask/brush_spatial_index.hpp"
#include "edit/mask/grade_mask_coverage.hpp"
#include "edit/mask/mask_model.hpp"
#include "grade_owned_mask_support.hpp"

#include <gtest/gtest.h>

#include <span>
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

auto CopyPixels(std::span<const std::uint8_t> pixels) -> std::vector<std::uint8_t> {
  return {pixels.begin(), pixels.end()};
}

}  // namespace

TEST(BrushPlacementMappingTest, BrushMoveThenDrawUsesTranslatedLocalCoordinates) {
  auto      grade = ColorGradeNodeModel::MakeClean(NodeId{"grade.primary"});
  MaskModel mask  = grade_mask_test::MakeParameterizedBrushMask(MaskId{"mask.brush"});
  grade->AddMask(std::move(mask), 0);
  const auto id = MaskId{"mask.brush"};

  const Vector2 first_world{20.0f, 16.0f};
  const Vector2 translation{8.0f, -4.0f};
  BrushCanonicalSampler first;
  first.BeginStroke(BrushStrokeMode::Paint, first_world, {}, 6.0f, 1.0f, 1.0f);
  const auto first_samples = first.FinishStroke();
  ASSERT_EQ(first_samples.size(), 1u);
  EXPECT_FLOAT_EQ(first_samples.front().local_x, first_world.x);
  EXPECT_FLOAT_EQ(first_samples.front().local_y, first_world.y);

  grade->AppendBrushStroke(
      {grade->Id(), id, MakeBrushStroke(StrokeId{"paint.first"}, BrushStrokeMode::Paint,
                                       first_samples),
       grade->MaskContentRevision(id)});
  const auto before = grade->BrushPlacementTranslation(id);
  EXPECT_EQ(before, Vector2{});
  const Vector2 drag_end = BrushWorldFromLocal(first_world, translation);
  EXPECT_EQ(BrushPlacementForReferenceDrag(before, first_world, drag_end), translation);
  const auto first_body = grade->BrushStrokes(id).front();
  grade->SetBrushTranslation(MakeBrushTranslationCommand(
      grade->Id(), id, before, translation, grade->MaskContentRevision(id)));
  EXPECT_EQ(grade->BrushPlacementTranslation(id), translation);
  EXPECT_TRUE(BrushStrokesShareSampleBody(first_body, grade->BrushStrokes(id).front()));
  EXPECT_FLOAT_EQ(grade->BrushStrokes(id).front().samples->front().local_x, first_world.x);
  EXPECT_FLOAT_EQ(grade->BrushStrokes(id).front().samples->front().local_y, first_world.y);

  const Vector2 second_world{40.0f, 20.0f};
  const auto    expected_local = BrushLocalFromReference(second_world, translation);
  EXPECT_FLOAT_EQ(expected_local.x, 32.0f);
  EXPECT_FLOAT_EQ(expected_local.y, 24.0f);
  BrushCanonicalSampler second;
  second.BeginStroke(BrushStrokeMode::Paint, second_world, translation, 6.0f, 1.0f, 1.0f);
  const auto second_samples = second.FinishStroke();
  ASSERT_EQ(second_samples.size(), 1u);
  EXPECT_FLOAT_EQ(second_samples.front().local_x, expected_local.x);
  EXPECT_FLOAT_EQ(second_samples.front().local_y, expected_local.y);

  grade->AppendBrushStroke(
      {grade->Id(), id, MakeBrushStroke(StrokeId{"paint.second"}, BrushStrokeMode::Paint,
                                       second_samples),
       grade->MaskContentRevision(id)});

  const auto* live = std::get_if<BrushMaskSource>(&grade->FindMask(id)->source);
  ASSERT_NE(live, nullptr);
  BrushRasterizer rasterizer;
  rasterizer.SetGeometry(kRaster, kFull);
  rasterizer.RasterizeFull(*live);
  const auto expected = brush_replay_oracle::BrushSourceR8(*live, kRaster, kFull);
  ExpectR8Equal(CopyPixels(rasterizer.Pixels()), expected, kRaster);

  EXPECT_EQ(rasterizer.Pixels()[PackedR8Index(28, 12, kRaster)], 255);
  EXPECT_EQ(rasterizer.Pixels()[PackedR8Index(40, 20, kRaster)], 255);
  EXPECT_EQ(rasterizer.Pixels()[PackedR8Index(20, 16, kRaster)], 0);
}

TEST(BrushPlacementMappingTest, RepeatedBrushMoveDoesNotBlurCoverage) {
  auto      grade = ColorGradeNodeModel::MakeClean(NodeId{"grade.primary"});
  MaskModel mask  = grade_mask_test::MakeParameterizedBrushMask(MaskId{"mask.brush"});
  grade->AddMask(std::move(mask), 0);
  const auto id = MaskId{"mask.brush"};
  const auto stroke =
      MakeBrushStroke(StrokeId{"paint"}, BrushStrokeMode::Paint,
                      {{24.0f, 18.0f, 8.0f, 1.0f, 1.0f}, {32.0f, 20.0f, 8.0f, 1.0f, 1.0f}});
  grade->AppendBrushStroke({grade->Id(), id, stroke, grade->MaskContentRevision(id)});
  const auto original_body = grade->BrushStrokes(id).front();

  GradeMaskCoverage coverage;
  coverage.SetGeometry(kRaster, kFull, kTile);
  auto source = std::get<BrushMaskSource>(grade->FindMask(id)->source);
  coverage.BindBrushSource(id, source);
  coverage.EvaluateFull(grade->Masks());
  const auto original_pixels = CopyPixels(coverage.Pixels());
  ExpectR8Equal(original_pixels, brush_replay_oracle::GradeMixR8(grade->Masks(), kRaster, kFull),
                kRaster);

  const Vector2 a{};
  const Vector2 b{10.0f, -6.0f};
  const auto    old_support = BrushSourceOutputTexelSupport(source, kRaster, kFull);
  grade->SetBrushTranslation(MakeBrushTranslationCommand(
      grade->Id(), id, a, b, grade->MaskContentRevision(id)));
  source = std::get<BrushMaskSource>(grade->FindMask(id)->source);
  const auto new_support = BrushSourceOutputTexelSupport(source, kRaster, kFull);
  coverage.ReplayRegion(grade->Masks(), UnionTexelRect(old_support, new_support));
  EXPECT_TRUE(BrushStrokesShareSampleBody(original_body, grade->BrushStrokes(id).front()));

  const auto mid_support = BrushSourceOutputTexelSupport(source, kRaster, kFull);
  grade->SetBrushTranslation(MakeBrushTranslationCommand(
      grade->Id(), id, b, a, grade->MaskContentRevision(id)));
  source = std::get<BrushMaskSource>(grade->FindMask(id)->source);
  EXPECT_EQ(source.placement_translation, a);
  coverage.ReplayRegion(grade->Masks(), UnionTexelRect(mid_support, old_support));
  const auto restored = CopyPixels(coverage.Pixels());
  ExpectR8Equal(restored, original_pixels, kRaster);
  ExpectR8Equal(restored, brush_replay_oracle::GradeMixR8(grade->Masks(), kRaster, kFull),
                kRaster);
  EXPECT_TRUE(BrushStrokesShareSampleBody(original_body, grade->BrushStrokes(id).front()));
  EXPECT_EQ(original_body.samples->front().local_x,
            grade->BrushStrokes(id).front().samples->front().local_x);
}

}  // namespace alcedo
