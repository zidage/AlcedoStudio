//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_mini_git_materializer.hpp"

#include <gtest/gtest.h>

#include <filesystem>

#include "edit/history/mini_git_working_history.hpp"
#include "support/editor_mini_git_project_fixture.hpp"

namespace alcedo {
namespace {

class EditorMiniGitMaterializerTest : public ::testing::Test {
 protected:
  void SetUp() override { project_.SetUp(); }
  void TearDown() override { project_.TearDown(); }

  test::EditorMiniGitProjectFixture project_;
};

TEST_F(EditorMiniGitMaterializerTest, EmptyJournalSucceedsWithoutMovingVersionHead) {
  const auto prior_head =
      project_.graph(test::EditorMiniGitProjectFixture::kElementA)->GetActiveVersionRef().head_commit_hash;
  auto        capture = project_.CaptureWorkingState(test::EditorMiniGitProjectFixture::kElementA, 0.0f);

  std::string error;
  const auto  result = project_.MaterializeUnderSaveLock(capture, &error);
  ASSERT_TRUE(result.accepted) << error << " / " << result.error;
  ASSERT_TRUE(result.materialized);
  EXPECT_FALSE(result.head_moved);

  auto stored = project_.LoadStoredGraph(test::EditorMiniGitProjectFixture::kElementA);
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->GetActiveVersionRef().head_commit_hash, prior_head);
  EXPECT_EQ(stored->CommitCount(), 0u);
}

TEST_F(EditorMiniGitMaterializerTest,
       MaterializeWritesCommitVersionAndSerializedStateThenTruncates) {
  ASSERT_TRUE(project_.AppendExposureEdit(test::EditorMiniGitProjectFixture::kElementA, 0.0f, 1.25f));
  auto capture = project_.CaptureWorkingState(test::EditorMiniGitProjectFixture::kElementA, 1.25f);
  ASSERT_FALSE(capture.journal_records.empty());
  ASSERT_TRUE(std::filesystem::exists(
      project_.journal_path(test::EditorMiniGitProjectFixture::kElementA)));

  std::string error;
  const auto  result = project_.MaterializeUnderSaveLock(capture, &error);
  ASSERT_TRUE(result.accepted) << error << " / " << result.error;
  ASSERT_TRUE(result.materialized);
  EXPECT_TRUE(result.head_moved);

  const auto records =
      project_.ReadJournalRecords(test::EditorMiniGitProjectFixture::kElementA, &error);
  EXPECT_TRUE(records.empty()) << error;

  auto stored = project_.LoadStoredGraph(test::EditorMiniGitProjectFixture::kElementA);
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->CommitCount(), 1u);
  EXPECT_EQ(stored->GetActiveVersionRef().head_commit_hash, capture.working_head);
  EXPECT_EQ(stored->GetImageEditState().materialized_transaction_chain_hash,
            capture.transaction_chain_hash);
  ASSERT_TRUE(stored->GetImageEditState().serialized_pipeline_state.has_value());
  EXPECT_FLOAT_EQ(stored->GetImageEditState()
                      .serialized_pipeline_state->at("pipeline_params")
                      .at("exposure")
                      .get<float>(),
                  1.25f);
}

TEST_F(EditorMiniGitMaterializerTest, FailureBeforeDuckDBCommitLeavesPriorHeadUnchanged) {
  ASSERT_TRUE(project_.AppendExposureEdit(test::EditorMiniGitProjectFixture::kElementA, 0.0f, 0.5f));
  auto capture         = project_.CaptureWorkingState(test::EditorMiniGitProjectFixture::kElementA, 0.5f);
  capture.working_head = Hash128{0xdead, 0xbeef};

  std::string error;
  const auto  result = project_.MaterializeUnderSaveLock(capture, &error);
  EXPECT_FALSE(result.accepted);
  EXPECT_FALSE(result.materialized);

  auto stored = project_.LoadStoredGraph(test::EditorMiniGitProjectFixture::kElementA);
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->CommitCount(), 0u);
  EXPECT_FALSE(stored->GetActiveVersionRef().head_commit_hash.has_value());
}

TEST_F(EditorMiniGitMaterializerTest, CrashAfterDuckDBBeforeTruncateDoesNotReplayCommitTwice) {
  ASSERT_TRUE(project_.AppendExposureEdit(test::EditorMiniGitProjectFixture::kElementA, 0.0f, 0.75f));
  auto        capture = project_.CaptureWorkingState(test::EditorMiniGitProjectFixture::kElementA, 0.75f);

  std::string error;
  ASSERT_TRUE(project_.MaterializeUnderSaveLock(capture, &error).accepted) << error;

  {
    MiniGitJournal leftover(
        project_.journal_path(test::EditorMiniGitProjectFixture::kElementA));
    for (const auto& record : capture.journal_records) {
      ASSERT_TRUE(leftover.Append(record, &error)) << error;
    }
  }

  const auto recovered = project_.materializer().RecoverAndMaterialize(
      test::EditorMiniGitProjectFixture::kElementA,
      project_.journal_path(test::EditorMiniGitProjectFixture::kElementA), &error);
  ASSERT_TRUE(recovered.accepted) << error << " / " << recovered.error;
  EXPECT_FALSE(recovered.head_moved);

  EXPECT_EQ(project_.CountStoredCommits(test::EditorMiniGitProjectFixture::kElementA), 1u);
  const auto records =
      project_.ReadJournalRecords(test::EditorMiniGitProjectFixture::kElementA, &error);
  EXPECT_TRUE(records.empty()) << error;
}

TEST_F(EditorMiniGitMaterializerTest, DistinctRootsKeepImageAAndImageBIsolated) {
  ASSERT_TRUE(project_.AppendExposureEdit(test::EditorMiniGitProjectFixture::kElementA, 0.0f, 1.0f));
  ASSERT_TRUE(project_.AppendExposureEdit(test::EditorMiniGitProjectFixture::kElementB, 0.0f, 2.0f));
  auto capture_a = project_.CaptureWorkingState(test::EditorMiniGitProjectFixture::kElementA, 1.0f);
  auto capture_b = project_.CaptureWorkingState(test::EditorMiniGitProjectFixture::kElementB, 2.0f);

  std::string error;
  ASSERT_TRUE(project_.MaterializeUnderSaveLock(capture_a, &error).accepted) << error;
  ASSERT_TRUE(project_.MaterializeUnderSaveLock(capture_b, &error).accepted) << error;

  EXPECT_NE(project_.root_id(test::EditorMiniGitProjectFixture::kElementA),
            project_.root_id(test::EditorMiniGitProjectFixture::kElementB));
  EXPECT_EQ(project_.CountStoredCommits(test::EditorMiniGitProjectFixture::kElementA), 1u);
  EXPECT_EQ(project_.CountStoredCommits(test::EditorMiniGitProjectFixture::kElementB), 1u);

  project_.CloseAndReopenProject();
  auto stored_a = project_.LoadStoredGraph(test::EditorMiniGitProjectFixture::kElementA);
  auto stored_b = project_.LoadStoredGraph(test::EditorMiniGitProjectFixture::kElementB);
  ASSERT_TRUE(stored_a.has_value());
  ASSERT_TRUE(stored_b.has_value());
  EXPECT_EQ(stored_a->CommitCount(), 1u);
  EXPECT_EQ(stored_b->CommitCount(), 1u);
}

}  // namespace
}  // namespace alcedo
