//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/mask/brush_coverage_update.hpp"

#include "edit/graph/graph_ids.hpp"
#include "edit/mask/brush_raster_encoding.hpp"
#include "edit/mask/brush_source_geometry.hpp"
#include "edit/mask/brush_stroke.hpp"
#include "edit/mask/mask_id.hpp"
#include "edit/mask/mask_model.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace alcedo {
namespace {

constexpr Extent2D kFull{96, 64};

auto Paint(std::string_view id, std::vector<BrushCanonicalSample> samples) -> BrushStroke {
  return MakeBrushStroke(StrokeId{std::string{id}}, BrushStrokeMode::Paint, std::move(samples));
}

auto MakeBrush(std::vector<BrushStroke> strokes, Vector2 translation = {}) -> BrushMaskSource {
  BrushMaskSource source;
  source.strokes               = std::move(strokes);
  source.placement_translation = translation;
  return source;
}

TEST(BrushCoverageUpdate, MissingPreviousReplaysFullRaster) {
  const auto next = MakeBrush({Paint("a", {{20.0f, 16.0f, 6.0f, 1.0f, 1.0f}})});
  const auto raster = CanonicalBrushRasterExtent(kFull);
  const auto update =
      DetectBrushCoverageUpdate(nullptr, {}, {}, next, raster, kFull);
  EXPECT_EQ(update.kind, BrushCoverageUpdateKind::ReplayDirty);
  EXPECT_EQ(update.dirty, FullTexelRect(raster));
}

TEST(BrushCoverageUpdate, EqualSourcesAreUnchanged) {
  const auto source = MakeBrush({Paint("a", {{20.0f, 16.0f, 6.0f, 1.0f, 1.0f}})});
  const auto raster = CanonicalBrushRasterExtent(kFull);
  const auto update =
      DetectBrushCoverageUpdate(&source, raster, kFull, source, raster, kFull);
  EXPECT_EQ(update.kind, BrushCoverageUpdateKind::Unchanged);
  EXPECT_EQ(update.dirty, (RectI{0, 0, 1, 1}));
}

TEST(BrushCoverageUpdate, GrownDraftStampsOnlyNewSamples) {
  auto previous = MakeBrush({Paint("draft", {{20.0f, 16.0f, 6.0f, 1.0f, 1.0f}})});
  auto next     = previous;
  next.strokes.back() = Paint("draft", {{20.0f, 16.0f, 6.0f, 1.0f, 1.0f},
                                        {70.0f, 50.0f, 5.0f, 1.0f, 1.0f}});
  const auto raster = CanonicalBrushRasterExtent(kFull);
  const auto update =
      DetectBrushCoverageUpdate(&previous, raster, kFull, next, raster, kFull);
  EXPECT_EQ(update.kind, BrushCoverageUpdateKind::StampNewSamples);
  EXPECT_EQ(update.stroke_index, 0u);
  EXPECT_EQ(update.stroke_end, 1u);
  EXPECT_EQ(update.sample_begin, 1u);
  EXPECT_EQ(update.dirty, BrushDabOutputTexelSupport(BrushStrokeSamples(next.strokes[0])[1],
                                                     next.placement_translation, raster, kFull));
}

TEST(BrushCoverageUpdate, AppendedStrokeStampsNewStroke) {
  auto previous = MakeBrush({Paint("a", {{20.0f, 16.0f, 6.0f, 1.0f, 1.0f}})});
  auto next     = previous;
  next.strokes.push_back(Paint("b", {{70.0f, 50.0f, 5.0f, 1.0f, 1.0f}}));
  const auto raster = CanonicalBrushRasterExtent(kFull);
  const auto update =
      DetectBrushCoverageUpdate(&previous, raster, kFull, next, raster, kFull);
  EXPECT_EQ(update.kind, BrushCoverageUpdateKind::StampNewSamples);
  EXPECT_EQ(update.stroke_index, 1u);
  EXPECT_EQ(update.stroke_end, 2u);
  EXPECT_EQ(update.sample_begin, 0u);
  EXPECT_EQ(update.dirty, BrushStrokeOutputTexelSupport(next.strokes[1], next.placement_translation,
                                                        raster, kFull));
}

TEST(BrushCoverageUpdate, PlacementMoveReplaysOldAndNewBounds) {
  const auto paint = Paint("a", {{30.0f, 20.0f, 8.0f, 1.0f, 1.0f}});
  auto       previous = MakeBrush({paint});
  auto       next     = previous;
  next.placement_translation = {12.0f, -6.0f};
  const auto raster = CanonicalBrushRasterExtent(kFull);
  const auto update =
      DetectBrushCoverageUpdate(&previous, raster, kFull, next, raster, kFull);
  EXPECT_EQ(update.kind, BrushCoverageUpdateKind::ReplayDirty);
  EXPECT_EQ(update.dirty,
            UnionTexelRect(BrushStrokeOutputTexelSupport(paint, previous.placement_translation,
                                                         raster, kFull),
                           BrushStrokeOutputTexelSupport(paint, next.placement_translation, raster,
                                                         kFull)));
}

TEST(BrushCoverageUpdate, RemovedStrokeReplaysOldRegion) {
  auto previous = MakeBrush({Paint("a", {{20.0f, 16.0f, 6.0f, 1.0f, 1.0f}}),
                             Paint("b", {{70.0f, 50.0f, 5.0f, 1.0f, 1.0f}})});
  auto next     = previous;
  const auto raster = CanonicalBrushRasterExtent(kFull);
  const auto removed =
      BrushStrokeOutputTexelSupport(next.strokes.back(), next.placement_translation, raster, kFull);
  next.strokes.pop_back();
  const auto update =
      DetectBrushCoverageUpdate(&previous, raster, kFull, next, raster, kFull);
  EXPECT_EQ(update.kind, BrushCoverageUpdateKind::ReplayDirty);
  EXPECT_EQ(update.dirty, removed);
}

TEST(BrushCoverageUpdate, FeatherRadiusChangeDoesNotStampCoverage) {
  auto previous = MakeBrush({Paint("a", {{20.0f, 16.0f, 6.0f, 1.0f, 1.0f}})});
  auto next     = previous;
  next.feather_radius = 4.0f;
  const auto raster  = CanonicalBrushRasterExtent(kFull);
  const auto update  =
      DetectBrushCoverageUpdate(&previous, raster, kFull, next, raster, kFull);
  EXPECT_EQ(update.kind, BrushCoverageUpdateKind::Unchanged);
}

TEST(BrushCoverageUpdate, EmptyThenFirstStrokeStampsTheNewStroke) {
  auto previous = MakeBrush({});
  auto next     = MakeBrush({Paint("a", {{20.0f, 16.0f, 6.0f, 1.0f, 1.0f}})});
  const auto raster = CanonicalBrushRasterExtent(kFull);
  const auto update =
      DetectBrushCoverageUpdate(&previous, raster, kFull, next, raster, kFull);
  EXPECT_EQ(update.kind, BrushCoverageUpdateKind::StampNewSamples);
  EXPECT_EQ(update.stroke_index, 0u);
  EXPECT_EQ(update.stroke_end, 1u);
  EXPECT_EQ(update.sample_begin, 0u);
}

TEST(BrushCoverageCommandJournal, StoreFindAndClearRoundTrip) {
  BrushCoverageCommandJournal journal;
  const NodeId grade{"grade.primary"};
  const MaskId mask{"mask.brush"};
  const auto   source = MakeBrush({Paint("a", {{20.0f, 16.0f, 6.0f, 1.0f, 1.0f}})});
  const auto   raster = CanonicalBrushRasterExtent(kFull);
  EXPECT_EQ(journal.Find(grade, mask), nullptr);
  journal.Store(grade, mask, source, raster, kFull);
  const auto* found = journal.Find(grade, mask);
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->source, source);
  EXPECT_EQ(found->raster, raster);
  EXPECT_EQ(found->full_reference, kFull);
  journal.Clear();
  EXPECT_EQ(journal.Find(grade, mask), nullptr);
}

}  // namespace
}  // namespace alcedo
