//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/mask/parameterized_brush_replay_cache.hpp"

#include "brush_replay_oracle.hpp"
#include "edit/mask/brush_raster_encoding.hpp"
#include "edit/mask/brush_source_geometry.hpp"
#include "edit/mask/brush_stroke.hpp"
#include "edit/mask/mask_model.hpp"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace alcedo {
namespace {

constexpr Extent2D kFull{96, 64};

const NodeId kGrade{"grade.primary"};
const MaskId kMask{"mask.brush"};

void ExpectPixelsMatchOracle(const ActiveRasterMaskInput& input, const BrushMaskSource& source,
                             Extent2D full_reference) {
  ASSERT_NE(input.pixels, nullptr);
  const auto raster = CanonicalBrushRasterExtent(full_reference);
  ASSERT_EQ(input.pixels->size(),
            static_cast<std::size_t>(raster.width) * raster.height);
  const auto expected = brush_replay_oracle::BrushSourceR8(source, raster, full_reference);
  for (std::uint32_t y = 0; y < raster.height; ++y) {
    for (std::uint32_t x = 0; x < raster.width; ++x) {
      const auto i = PackedR8Index(x, y, raster);
      ASSERT_EQ((*input.pixels)[i], expected[i]) << "texel " << x << "," << y;
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

auto StrokeSupport(const BrushStroke& stroke, Vector2 translation, Extent2D raster,
                   Extent2D full_reference) -> RectI {
  return BrushStrokeOutputTexelSupport(stroke, translation, raster, full_reference);
}

TEST(ParameterizedBrushReplayCache, FirstReplayMatchesFullRasterization) {
  ParameterizedBrushReplayCache cache;
  const auto source = MakeBrush({
      PaintStroke("paint.a", {{12.0f, 10.0f, 6.0f, 1.0f, 1.0f},
                              {18.0f, 12.0f, 6.0f, 0.75f, 0.5f}}),
      EraseStroke("erase.b", {{16.0f, 11.0f, 4.0f, 1.0f, 1.0f}}),
  });
  const auto input = cache.Replay(kGrade, kMask, source, kFull, 7);
  EXPECT_EQ(input.content_revision, 7u);
  EXPECT_EQ(input.descriptor.extent, CanonicalBrushRasterExtent(kFull));
  EXPECT_EQ(input.dirty_rectangle, FullTexelRect(input.descriptor.extent));
  ExpectPixelsMatchOracle(input, source, kFull);
}

TEST(ParameterizedBrushReplayCache, UnchangedSourceReusesPixelsAndSkipsReplay) {
  ParameterizedBrushReplayCache cache;
  const auto source =
      MakeBrush({PaintStroke("paint", {{30.0f, 20.0f, 8.0f, 1.0f, 1.0f}})});
  const auto first  = cache.Replay(kGrade, kMask, source, kFull, 1);
  const auto second = cache.Replay(kGrade, kMask, source, kFull, 2);
  // No dab changed: the retained buffer is returned untouched, so the shared
  // handle is identical and no rasterization ran.
  EXPECT_EQ(second.pixels.get(), first.pixels.get());
  // The upload contract still names a non-empty clipped rectangle.
  EXPECT_EQ(second.dirty_rectangle, (RectI{0, 0, 1, 1}));
  EXPECT_EQ(second.content_revision, 2u);
}

TEST(ParameterizedBrushReplayCache, AppendedStrokeReplaysOnlyItsSupport) {
  ParameterizedBrushReplayCache cache;
  auto source = MakeBrush({PaintStroke("paint.a", {{20.0f, 16.0f, 6.0f, 1.0f, 1.0f}})});
  (void)cache.Replay(kGrade, kMask, source, kFull, 1);

  const auto added = PaintStroke("paint.b", {{70.0f, 50.0f, 5.0f, 1.0f, 1.0f}});
  source.strokes.push_back(added);
  const auto input = cache.Replay(kGrade, kMask, source, kFull, 2);
  EXPECT_EQ(input.dirty_rectangle,
            StrokeSupport(added, source.placement_translation, input.descriptor.extent, kFull));
  ExpectPixelsMatchOracle(input, source, kFull);
}

TEST(ParameterizedBrushReplayCache, RegrownDraftStrokeStampsOnlyNewDabSupport) {
  ParameterizedBrushReplayCache cache;
  const std::vector<BrushCanonicalSample> first_dabs{{20.0f, 16.0f, 6.0f, 1.0f, 1.0f}};
  auto source = MakeBrush({PaintStroke("draft", first_dabs)});
  (void)cache.Replay(kGrade, kMask, source, kFull, 1);

  std::vector<BrushCanonicalSample> grown = first_dabs;
  grown.push_back({70.0f, 50.0f, 5.0f, 1.0f, 1.0f});
  source.strokes.back() =
      MakeBrushStroke(StrokeId{"draft"}, BrushStrokeMode::Paint, std::move(grown));
  const auto input = cache.Replay(kGrade, kMask, source, kFull, 2);
  const auto raster = CanonicalBrushRasterExtent(kFull);
  EXPECT_EQ(input.dirty_rectangle,
            BrushDabOutputTexelSupport(source.strokes.back().samples->back(),
                                       source.placement_translation, raster, kFull));
  ExpectPixelsMatchOracle(input, source, kFull);
}

TEST(ParameterizedBrushReplayCache, PlacementMoveReplaysOldAndNewBounds) {
  ParameterizedBrushReplayCache cache;
  const auto paint = PaintStroke("paint", {{30.0f, 20.0f, 8.0f, 1.0f, 1.0f}});
  auto       source = MakeBrush({paint});
  (void)cache.Replay(kGrade, kMask, source, kFull, 1);

  const auto before = StrokeSupport(paint, source.placement_translation,
                                    CanonicalBrushRasterExtent(kFull), kFull);
  source.placement_translation = {12.0f, -6.0f};
  const auto after = StrokeSupport(paint, source.placement_translation,
                                   CanonicalBrushRasterExtent(kFull), kFull);
  const auto input = cache.Replay(kGrade, kMask, source, kFull, 2);
  EXPECT_EQ(input.dirty_rectangle, UnionTexelRect(before, after));
  ExpectPixelsMatchOracle(input, source, kFull);
}

TEST(ParameterizedBrushReplayCache, EraseAppendAltersActualCoverage) {
  ParameterizedBrushReplayCache cache;
  auto source = MakeBrush({PaintStroke("paint", {{24.0f, 18.0f, 10.0f, 1.0f, 1.0f}})});
  const auto painted = cache.Replay(kGrade, kMask, source, kFull, 1);
  const auto center =
      PackedR8Index(24, 18, CanonicalBrushRasterExtent(kFull));
  // The retained buffer is shared: capture coverage before the erase replay
  // mutates it in place.
  const auto painted_value = (*painted.pixels)[center];
  ASSERT_GT(painted_value, 0);

  source.strokes.push_back(EraseStroke("erase", {{24.0f, 18.0f, 10.0f, 1.0f, 1.0f}}));
  const auto erased = cache.Replay(kGrade, kMask, source, kFull, 2);
  EXPECT_LT((*erased.pixels)[center], painted_value);
  ExpectPixelsMatchOracle(erased, source, kFull);
}

TEST(ParameterizedBrushReplayCache, RemovedStrokeReplaysItsOldRegion) {
  ParameterizedBrushReplayCache cache;
  auto source = MakeBrush({PaintStroke("a", {{20.0f, 16.0f, 6.0f, 1.0f, 1.0f}}),
                           PaintStroke("b", {{70.0f, 50.0f, 5.0f, 1.0f, 1.0f}})});
  (void)cache.Replay(kGrade, kMask, source, kFull, 1);

  const auto removed_support = StrokeSupport(source.strokes.back(), source.placement_translation,
                                             CanonicalBrushRasterExtent(kFull), kFull);
  source.strokes.pop_back();
  const auto input = cache.Replay(kGrade, kMask, source, kFull, 2);
  EXPECT_EQ(input.dirty_rectangle, removed_support);
  ExpectPixelsMatchOracle(input, source, kFull);
}

TEST(ParameterizedBrushReplayCache, GeometryChangeRestartsWithFreshBuffer) {
  ParameterizedBrushReplayCache cache;
  const auto source =
      MakeBrush({PaintStroke("paint", {{30.0f, 20.0f, 8.0f, 1.0f, 1.0f}})});
  const auto first = cache.Replay(kGrade, kMask, source, kFull, 1);
  const auto first_bytes = *first.pixels;

  const Extent2D larger{192, 128};
  const auto     second = cache.Replay(kGrade, kMask, source, larger, 2);
  EXPECT_EQ(second.descriptor.extent, CanonicalBrushRasterExtent(larger));
  EXPECT_EQ(second.dirty_rectangle, FullTexelRect(second.descriptor.extent));
  // The previous handle keeps its old bytes: the retained buffer is replaced,
  // not mutated under a live consumer.
  EXPECT_EQ(*first.pixels, first_bytes);
  ExpectPixelsMatchOracle(second, source, larger);
}

TEST(ParameterizedBrushReplayCache, EvictedEntryRerasterizesToMatchingPixels) {
  ParameterizedBrushReplayCache cache;
  const auto source =
      MakeBrush({PaintStroke("paint", {{30.0f, 20.0f, 8.0f, 1.0f, 1.0f}})});
  // Fill past the retained-entry bound so the first key is evicted.
  for (std::size_t i = 0; i <= ParameterizedBrushReplayCache::kMaxRetainedEntries; ++i) {
    const MaskId other{std::string{"mask."} + std::to_string(i)};
    (void)cache.Replay(kGrade, other, source, kFull, 1);
  }
  const auto replayed = cache.Replay(kGrade, kMask, source, kFull, 9);
  ExpectPixelsMatchOracle(replayed, source, kFull);
  EXPECT_EQ(replayed.dirty_rectangle, FullTexelRect(replayed.descriptor.extent));
}

TEST(ParameterizedBrushReplayCache, EmptySourceRasterizesZerosAndBadVersionThrows) {
  ParameterizedBrushReplayCache cache;
  auto empty = MakeBrush({});
  const auto zeroed = cache.Replay(kGrade, kMask, empty, kFull, 1);
  for (const auto texel : *zeroed.pixels) {
    EXPECT_EQ(texel, 0);
  }
  auto bad = MakeBrush({PaintStroke("p", {{8.0f, 8.0f, 4.0f, 1.0f, 1.0f}})});
  bad.raster_algorithm_version = 99;
  EXPECT_THROW((void)cache.Replay(kGrade, MaskId{"mask.bad"}, bad, kFull, 1), std::runtime_error);
}

}  // namespace
}  // namespace alcedo
