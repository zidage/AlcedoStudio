//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/mask/brush_canonical_sampler.hpp"
#include "edit/mask/brush_raster_encoding.hpp"
#include "edit/mask/brush_stroke.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <vector>

namespace alcedo {
namespace {

auto SamplePath(const std::vector<Vector2>& points, Vector2 translation, float radius)
    -> std::vector<BrushCanonicalSample> {
  BrushCanonicalSampler sampler;
  sampler.BeginStroke(BrushStrokeMode::Paint, points.front(), translation, radius, 1.0f, 1.0f);
  for (std::size_t i = 1; i < points.size(); ++i) {
    sampler.AppendReference(points[i]);
  }
  return sampler.FinishStroke();
}

TEST(BrushCanonicalSampler, BeginEmitsPressAndFinishAddsEndpointOnce) {
  BrushCanonicalSampler sampler;
  sampler.BeginStroke(BrushStrokeMode::Paint, {4.0f, 6.0f}, {1.0f, 2.0f}, 8.0f, 1.0f, 1.0f);
  const auto click = sampler.FinishStroke();
  ASSERT_EQ(click.size(), 1u);
  EXPECT_FLOAT_EQ(click.front().local_x, 3.0f);
  EXPECT_FLOAT_EQ(click.front().local_y, 4.0f);
  EXPECT_FALSE(sampler.IsOpen());
}

TEST(BrushCanonicalSampler, EventBatchesKeepRemainderAndMatchDenseColinearPath) {
  const Vector2 start{10.0f, 12.0f};
  const Vector2 end{30.0f, 12.0f};
  const float   radius = 8.0f;
  std::vector<Vector2> sparse{start, end};
  std::vector<Vector2> grouped{start, {16.0f, 12.0f}, {22.0f, 12.0f}, end};
  std::vector<Vector2> dense{start};
  for (int i = 1; i <= 40; ++i) {
    dense.push_back({start.x + static_cast<float>(i) * 0.5f, start.y});
  }
  const auto a = SamplePath(sparse, {}, radius);
  const auto b = SamplePath(grouped, {}, radius);
  const auto c = SamplePath(dense, {}, radius);
  ASSERT_GE(a.size(), 2u);
  EXPECT_EQ(a, b);
  EXPECT_EQ(a, c);
  EXPECT_FLOAT_EQ(a.front().local_x, start.x);
  EXPECT_FLOAT_EQ(a.back().local_x, end.x);
}

TEST(BrushCanonicalSampler, ParameterBoundaryKeepsSizeChangeAtCurrentPoint) {
  BrushCanonicalSampler sampler;
  sampler.BeginStroke(BrushStrokeMode::Erase, {0.0f, 0.0f}, {}, 8.0f, 1.0f, 1.0f);
  sampler.SetSampleParameters(4.0f, 0.5f, 0.25f);
  sampler.AppendReference({20.0f, 0.0f});
  const auto samples = sampler.FinishStroke();
  ASSERT_GE(samples.size(), 2u);
  EXPECT_FLOAT_EQ(samples[1].radius, 4.0f);
  EXPECT_FLOAT_EQ(samples[1].strength, 0.5f);
  EXPECT_FLOAT_EQ(samples[1].hardness, 0.25f);
  EXPECT_EQ(sampler.Mode(), BrushStrokeMode::Erase);
}

TEST(BrushCanonicalSampler, DraftSamplesExposeOpenStrokeWithoutSealing) {
  BrushCanonicalSampler sampler;
  sampler.BeginStroke(BrushStrokeMode::Paint, {4.0f, 6.0f}, {}, 8.0f, 1.0f, 1.0f);
  const auto draft = sampler.DraftSamples();
  ASSERT_EQ(draft.size(), 1u);
  EXPECT_FLOAT_EQ(draft.front().local_x, 4.0f);
  sampler.AppendReference({12.0f, 6.0f});
  EXPECT_GE(sampler.DraftSamples().size(), 1u);
  const auto finished = sampler.FinishStroke();
  EXPECT_TRUE(sampler.DraftSamples().empty());
  EXPECT_GE(finished.size(), 2u);
}

TEST(BrushCanonicalSampler, CancelAndInvalidInputLeaveNoOpenStroke) {
  BrushCanonicalSampler sampler;
  EXPECT_THROW(sampler.AppendReference({1.0f, 1.0f}), std::runtime_error);
  sampler.BeginStroke(BrushStrokeMode::Paint, {0.0f, 0.0f}, {}, 4.0f, 1.0f, 1.0f);
  EXPECT_THROW(sampler.BeginStroke(BrushStrokeMode::Paint, {1.0f, 1.0f}, {}, 4.0f, 1.0f, 1.0f),
               std::runtime_error);
  sampler.CancelStroke();
  EXPECT_FALSE(sampler.IsOpen());
  EXPECT_THROW(sampler.BeginStroke(BrushStrokeMode::Paint, {0.0f, 0.0f}, {}, 0.0f, 1.0f, 1.0f),
               std::runtime_error);
}

}  // namespace
}  // namespace alcedo
