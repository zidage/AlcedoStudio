//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/mask/brush_source_geometry.hpp"
#include "edit/mask/brush_spatial_index.hpp"
#include "edit/mask/brush_stroke.hpp"
#include "edit/mask/mask_model.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

namespace alcedo {
namespace {

auto MakeSource(std::vector<BrushStroke> strokes, Vector2 translation = {}) -> BrushMaskSource {
  BrushMaskSource source;
  source.strokes               = std::move(strokes);
  source.placement_translation = translation;
  return source;
}

TEST(BrushSpatialIndex, WindingStrokeDoesNotMarkInteriorTiles) {
  const Extent2D raster{80, 80};
  const Extent2D full{80, 80};
  BrushSpatialIndex index(16);
  const auto stroke = MakeBrushStroke(
      StrokeId{"u"}, BrushStrokeMode::Paint,
      {{8.0f, 8.0f, 3.0f, 1.0f, 1.0f},
       {8.0f, 72.0f, 3.0f, 1.0f, 1.0f},
       {72.0f, 72.0f, 3.0f, 1.0f, 1.0f}});
  index.Rebuild(MakeSource({stroke}), raster, full);
  ASSERT_EQ(index.Spans().size(), 3u);
  for (const auto& span : index.Spans()) {
    EXPECT_FALSE(span.stroke_id.Empty());
    EXPECT_EQ(span.sample_end, span.sample_begin + 1);
    EXPECT_FALSE(RectIEmpty(span.local_bounds));
  }
  const auto interior = index.QueryLocal({32, 8, 16, 16});
  EXPECT_TRUE(interior.empty());
  const auto corner = index.QueryLocal({0, 0, 16, 16});
  ASSERT_EQ(corner.size(), 1u);
  EXPECT_EQ(corner.front().sample_begin, 0u);
}

TEST(BrushSpatialIndex, TranslationQueryUsesLocalIndexWithoutRebuild) {
  const Extent2D raster{64, 32};
  const Extent2D full{64, 32};
  BrushSpatialIndex index(16);
  const auto stroke = MakeBrushStroke(StrokeId{"a"}, BrushStrokeMode::Paint,
                                      {{8.0f, 8.0f, 4.0f, 1.0f, 1.0f}});
  index.Rebuild(MakeSource({stroke}), raster, full);
  const auto local = index.QueryLocal({0, 0, 16, 16});
  ASSERT_EQ(local.size(), 1u);
  const Vector2 translation{16.0f, 0.0f};
  const auto moved = index.QueryOutput({16, 0, 16, 16}, translation);
  ASSERT_EQ(moved.size(), 1u);
  EXPECT_EQ(moved.front().stroke_id, local.front().stroke_id);
  EXPECT_EQ(moved.front().local_bounds, local.front().local_bounds);
  const auto unmoved = index.QueryOutput({0, 0, 16, 16}, translation);
  EXPECT_TRUE(unmoved.empty());
}

TEST(BrushSpatialIndex, RejectsZeroTileSizeAndUnsupportedAlgorithm) {
  EXPECT_THROW(BrushSpatialIndex{0}, std::runtime_error);
  BrushSpatialIndex index(8);
  BrushMaskSource   source;
  source.raster_algorithm_version = 2;
  source.strokes                  = {MakeBrushStroke(StrokeId{"a"}, BrushStrokeMode::Paint,
                                                     {{4.0f, 4.0f, 2.0f, 1.0f, 1.0f}})};
  EXPECT_THROW(index.Rebuild(source, {16, 16}, {16, 16}), std::runtime_error);
}

}  // namespace
}  // namespace alcedo
