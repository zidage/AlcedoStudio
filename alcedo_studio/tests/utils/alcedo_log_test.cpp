//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <cstddef>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "utils/logging/log.hpp"

namespace alcedo::log {
namespace {

struct CapturedRecord {
  Level       level;
  Category    category;
  std::string file;
  int         line;
  std::string message;
};

std::vector<CapturedRecord> g_records;

void                        CaptureRecord(const Record& record) {
  g_records.push_back(CapturedRecord{record.level, record.category,
                                     record.file != nullptr ? record.file : "", record.line,
                                     std::string(record.message)});
}

class AlcedoLogTest : public ::testing::Test {
 protected:
  void SetUp() override {
    g_records.clear();
    ResetEnabledLevels();
  }
  void TearDown() override {
    SetHandler(nullptr);
    ResetEnabledLevels();
  }
};

TEST_F(AlcedoLogTest, CategoryNamesComeFromTheSharedList) {
  EXPECT_EQ(CategoryName(Category::App), "alcedo.app");
  EXPECT_EQ(CategoryName(Category::RenderRoi), "alcedo.render.roi");
  EXPECT_EQ(CategoryName(Category::GpuPool), "alcedo.gpu.pool");
  EXPECT_EQ(CategoryName(Category::EditorPresent), "alcedo.editor.present");
  EXPECT_EQ(CategoryName(Category::Update), "alcedo.update");
  EXPECT_EQ(static_cast<std::size_t>(Category::Update) + 1, kCategoryCount);
}

TEST_F(AlcedoLogTest, DefaultLevelsEnableInfoAndAboveAndDisableDebug) {
  for (std::size_t index = 0; index < kCategoryCount; ++index) {
    const auto category = static_cast<Category>(index);
    EXPECT_FALSE(IsEnabled(Level::Debug, category)) << CategoryName(category);
    EXPECT_TRUE(IsEnabled(Level::Info, category)) << CategoryName(category);
    EXPECT_TRUE(IsEnabled(Level::Warn, category)) << CategoryName(category);
    EXPECT_TRUE(IsEnabled(Level::Error, category)) << CategoryName(category);
  }
}

TEST_F(AlcedoLogTest, MessageReachesTheHandlerOnceWithCategoryAndSourceLocation) {
  SetHandler(CaptureRecord);
  const int expected_line = __LINE__ + 1;
  ALCEDO_LOG_WARN(Thumbnail) << "cache write failed: " << 42 << " bytes";

  ASSERT_EQ(g_records.size(), 1u);
  const auto& record = g_records.front();
  EXPECT_EQ(record.level, Level::Warn);
  EXPECT_EQ(record.category, Category::Thumbnail);
  EXPECT_EQ(record.message, "cache write failed: 42 bytes");
  EXPECT_NE(record.file.find("alcedo_log_test.cpp"), std::string::npos) << record.file;
  EXPECT_EQ(record.line, expected_line);
}

TEST_F(AlcedoLogTest, DisabledLevelDoesNotEvaluateItsArguments) {
  SetHandler(CaptureRecord);
  SetEnabled(Category::Lens, Level::Info, false);

  int evaluations = 0;
  ALCEDO_LOG_INFO(Lens) << "evaluated " << ++evaluations;
  ALCEDO_LOG_INFO(Render) << "other category " << ++evaluations;

  EXPECT_EQ(evaluations, 1);
  ASSERT_EQ(g_records.size(), 1u);
  EXPECT_EQ(g_records.front().category, Category::Render);
}

TEST_F(AlcedoLogTest, EnabledDebugLevelReachesTheHandlerInDebugBuilds) {
  SetHandler(CaptureRecord);
  SetEnabled(Category::RenderRoi, Level::Debug, true);

  ALCEDO_LOG_DEBUG(RenderRoi) << "roi";

  if constexpr (kCompiledMinLevel == Level::Debug) {
    ASSERT_EQ(g_records.size(), 1u);
    EXPECT_EQ(g_records.front().level, Level::Debug);
  } else {
    EXPECT_TRUE(g_records.empty());
  }
}

TEST_F(AlcedoLogTest, MacroIsOneStatementInsideIfElse) {
  SetHandler(CaptureRecord);
  bool       else_branch = false;
  const bool condition   = false;
  if (condition)
    ALCEDO_LOG_ERROR(App) << "not written";
  else
    else_branch = true;

  EXPECT_TRUE(else_branch);
  EXPECT_TRUE(g_records.empty());
}

TEST_F(AlcedoLogTest, DefaultHandlerWritesCompleteLinesFromEightThreads) {
  constexpr int     kThreads        = 8;
  constexpr int     kLinesPerThread = 10'000;
  // A long payload makes a torn write likely if lines were not written in one call.
  const std::string payload(64, 'x');

  ::testing::internal::CaptureStderr();
  {
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int thread = 0; thread < kThreads; ++thread) {
      threads.emplace_back([thread, &payload] {
        for (int line = 0; line < kLinesPerThread; ++line) {
          ALCEDO_LOG_INFO(App) << "thread=" << thread << " line=" << line << " payload=" << payload;
        }
      });
    }
    for (auto& thread : threads) {
      thread.join();
    }
  }
  const std::string output = ::testing::internal::GetCapturedStderr();

  const std::regex  complete_line(
      R"(^\[INFO\] \[alcedo\.app\] thread=(\d) line=(\d+) payload=x{64} \(alcedo_log_test\.cpp:\d+\)\r?$)");
  std::istringstream            lines(output);
  std::string                   text;
  std::set<std::pair<int, int>> seen;
  std::size_t                   line_count = 0;
  while (std::getline(lines, text)) {
    ++line_count;
    std::smatch match;
    ASSERT_TRUE(std::regex_match(text, match, complete_line)) << "torn line: " << text;
    seen.emplace(std::stoi(match[1].str()), std::stoi(match[2].str()));
  }
  EXPECT_EQ(line_count, static_cast<std::size_t>(kThreads * kLinesPerThread));
  EXPECT_EQ(seen.size(), static_cast<std::size_t>(kThreads * kLinesPerThread));
}

}  // namespace
}  // namespace alcedo::log
