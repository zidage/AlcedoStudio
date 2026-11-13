//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// This translation unit compiles with the floor of Release and RelWithDebInfo builds (Info) in
// every build type, so the test of the compiled-out Debug level runs in the Debug test build too.
#undef ALCEDO_LOG_MIN_LEVEL
#define ALCEDO_LOG_MIN_LEVEL 1

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "utils/logging/log.hpp"

namespace alcedo::log {
namespace {

std::vector<std::string> g_messages;

void CaptureMessage(const Record& record) { g_messages.emplace_back(record.message); }

class AlcedoLogReleaseFloorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    g_messages.clear();
    ResetEnabledLevels();
    SetHandler(CaptureMessage);
  }
  void TearDown() override {
    SetHandler(nullptr);
    ResetEnabledLevels();
  }
};

TEST_F(AlcedoLogReleaseFloorTest, DebugMessageBelowInfoFloorDoesNotEvaluateItsArguments) {
  static_assert(kCompiledMinLevel == Level::Info);
  // Debug enabled at run time: only the compile-time floor can stop the message.
  SetEnabled(Category::Render, Level::Debug, true);

  int evaluations = 0;
  ALCEDO_LOG_DEBUG(Render) << "evaluated " << ++evaluations;

  EXPECT_EQ(evaluations, 0);
  EXPECT_TRUE(g_messages.empty());
}

TEST_F(AlcedoLogReleaseFloorTest, InfoMessageAtInfoFloorReachesTheHandler) {
  int evaluations = 0;
  ALCEDO_LOG_INFO(Render) << "evaluated " << ++evaluations;

  EXPECT_EQ(evaluations, 1);
  ASSERT_EQ(g_messages.size(), 1u);
  EXPECT_EQ(g_messages.front(), "evaluated 1");
}

}  // namespace
}  // namespace alcedo::log
