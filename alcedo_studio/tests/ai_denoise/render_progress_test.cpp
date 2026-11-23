//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Tests of the step values of a render (AI Denoise integration plan, DN14).

#include "edit/runtime/render_progress.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace alcedo {
namespace {

TEST(RenderProgressTest, ReporterSendsTheStepsAndTheDevelopGroups) {
  std::vector<RenderProgress> sent;
  RenderProgressReporter      reporter([&](const RenderProgress& value) { sent.push_back(value); },
                                  /*read_raw=*/true, /*ai_denoise=*/true);
  ASSERT_TRUE(reporter.Active());

  reporter.ReadRawStarted();
  reporter.AiDenoiseTiles(0, 0);
  reporter.AiDenoiseTiles(1, 2);
  reporter.AiDenoiseTiles(2, 2);
  const int groups = DevelopGroupCount(2);
  ASSERT_EQ(groups, 6);
  reporter.DevelopGroupStarted(DevelopGroup::Sensor, 0, groups);
  reporter.DevelopGroupStarted(DevelopGroup::ColorGrade, 4, groups, 1, 2);
  reporter.DevelopGroupStarted(DevelopGroup::Display, 5, groups);

  ASSERT_EQ(sent.size(), 7U);
  for (const auto& value : sent) {
    EXPECT_TRUE(value.read_raw);
    EXPECT_TRUE(value.ai_denoise);
  }
  EXPECT_EQ(sent[0].step, RenderStep::ReadRaw);
  EXPECT_EQ(sent[0].total, 0);
  EXPECT_EQ(sent[1].step, RenderStep::AiDenoise);
  EXPECT_EQ(sent[1].completed, 0);
  EXPECT_EQ(sent[1].total, 0);
  EXPECT_EQ(sent[3].completed, 2);
  EXPECT_EQ(sent[3].total, 2);
  EXPECT_EQ(sent[4].step, RenderStep::Develop);
  EXPECT_EQ(sent[4].group, DevelopGroup::Sensor);
  EXPECT_EQ(sent[4].completed, 0);
  EXPECT_EQ(sent[4].total, 6);
  EXPECT_EQ(sent[5].group, DevelopGroup::ColorGrade);
  EXPECT_EQ(sent[5].completed, 4);
  EXPECT_EQ(sent[5].grade_index, 1);
  EXPECT_EQ(sent[5].grade_count, 2);
  // A group after a Color Grade has no grade.
  EXPECT_EQ(sent[6].group, DevelopGroup::Display);
  EXPECT_EQ(sent[6].grade_index, 0);
  EXPECT_EQ(sent[6].grade_count, 0);
}

TEST(RenderProgressTest, RenderThatReadsOnlyTheCachesReportsNothing) {
  int                    sent = 0;
  RenderProgressReporter cached([&](const RenderProgress&) { ++sent; }, false, false);
  EXPECT_FALSE(cached.Active());
  cached.ReadRawStarted();
  cached.AiDenoiseTiles(1, 1);
  cached.DevelopGroupStarted(DevelopGroup::Sensor, 0, 4);
  EXPECT_EQ(sent, 0);

  // Without a receiver nothing is sent either.
  RenderProgressReporter without(RenderProgressCallback{}, true, false);
  EXPECT_FALSE(without.Active());
  without.ReadRawStarted();

  // A render of one step (the switch on, the source in the cache).
  std::vector<RenderProgress> values;
  RenderProgressReporter model_only([&](const RenderProgress& v) { values.push_back(v); }, false,
                                    true);
  model_only.AiDenoiseTiles(0, 0);
  ASSERT_EQ(values.size(), 1U);
  EXPECT_FALSE(values[0].read_raw);
  EXPECT_TRUE(values[0].ai_denoise);
}

}  // namespace
}  // namespace alcedo
