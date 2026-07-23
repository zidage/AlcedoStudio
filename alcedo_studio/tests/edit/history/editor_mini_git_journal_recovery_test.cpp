//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_mini_git_journal_recovery.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "app/editor_mini_git_commit_writer.hpp"
#include "app/editor_mini_git_journal_fold.hpp"
#include "app/project_service.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "storage/service/sleeve/edit_history/commit_graph_service.hpp"
#include "utils/clock/time_provider.hpp"

namespace alcedo {
namespace {

auto MakeExposurePayload(float before, float after) -> OrdinaryEditPayload {
  OrdinaryEditPayload payload;
  payload.operator_type  = OperatorType::EXPOSURE;
  payload.stage_name     = PipelineStageName::Basic_Adjustment;
  payload.field_name     = "$operator_params";
  payload.before_value   = nlohmann::json{{"exposure", before}};
  payload.after_value    = nlohmann::json{{"exposure", after}};
  payload.before_enabled = true;
  payload.after_enabled  = true;
  return payload;
}

}  // namespace

class EditorMiniGitJournalRecoveryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    TimeProvider::Refresh();
    RegisterAllOperators();
    const auto stamp =
        std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const auto temp = std::filesystem::temp_directory_path();
    db_path_        = temp / ("mini_git_recovery_" + stamp + ".db");
    meta_path_      = temp / ("mini_git_recovery_" + stamp + ".json");
    journal_path_   = temp / ("mini_git_recovery_" + stamp + ".mini-git.wal");
    std::error_code ec;
    std::filesystem::remove(db_path_, ec);
    std::filesystem::remove(meta_path_, ec);
    std::filesystem::remove(journal_path_, ec);

    project_  = std::make_unique<ProjectService>(db_path_, meta_path_);
    storage_  = project_->GetStorageService();
    recovery_ = std::make_unique<EditorMiniGitJournalRecovery>(storage_);
    {
      auto               guard = storage_->GetDBController().GetConnectionGuard();
      auto               lock  = guard.Lock();
      CommitGraphService graph_service(guard.conn_);
      graph_ =
          std::make_shared<CommitGraph>(graph_service.CreateEmptyPersisted(element_id_, "Default"));
    }
  }

  void TearDown() override {
    recovery_.reset();
    storage_.reset();
    project_.reset();
    std::error_code ec;
    std::filesystem::remove(db_path_, ec);
    std::filesystem::remove(meta_path_, ec);
    std::filesystem::remove(journal_path_, ec);
  }

  sl_element_id_t                               element_id_ = 42;
  std::filesystem::path                         db_path_;
  std::filesystem::path                         meta_path_;
  std::filesystem::path                         journal_path_;
  std::unique_ptr<ProjectService>               project_;
  std::shared_ptr<StorageService>               storage_;
  std::shared_ptr<CommitGraph>                  graph_;
  std::unique_ptr<EditorMiniGitJournalRecovery> recovery_;
};

TEST_F(EditorMiniGitJournalRecoveryTest, EmptyJournalSucceedsWithoutMovingHead) {
  // Create an empty journal file.
  {
    std::ofstream output(journal_path_, std::ios::binary | std::ios::trunc);
    output.close();
  }

  std::string error;
  auto        result = recovery_->Recover(element_id_, journal_path_, &error);
  EXPECT_TRUE(result.accepted) << result.error;
  EXPECT_FALSE(result.materialized);
}

TEST_F(EditorMiniGitJournalRecoveryTest, NonExistentJournalSucceeds) {
  std::string error;
  auto        result = recovery_->Recover(element_id_, journal_path_, &error);
  EXPECT_TRUE(result.accepted) << result.error;
  EXPECT_FALSE(result.materialized);
}

TEST_F(EditorMiniGitJournalRecoveryTest, AlreadyMaterializedJournalDoesNotDuplicateCommits) {
  // Write a journal with one edit, then materialize it, then simulate leftover
  // journal records after a crash-before-truncate.
  auto                  journal = std::make_shared<MiniGitJournal>(journal_path_);
  MiniGitWorkingHistory history(graph_, journal);
  ASSERT_TRUE(history.AppendEdit(MakeExposurePayload(0.0f, 0.75f)).committed);
  ASSERT_FALSE(journal->records().empty());

  // Materialize the same records via the CommitWriter (simulating a prior save).
  {
    auto        fold_graph = *graph_;
    std::string fold_error;
    auto fold_result = EditorMiniGitJournalFold::Fold(fold_graph, journal->records(), &fold_error);
    ASSERT_TRUE(fold_result.accepted) << fold_result.error;
    auto materialization = fold_graph.CaptureMaterializationWithSerializedPipelineState(
        fold_graph.GetImageEditState().serialized_pipeline_state);
    EditorMiniGitCommitWriter writer(storage_);
    auto                      write_result = writer.Write(materialization, &fold_error);
    ASSERT_TRUE(write_result.accepted) << write_result.error;
  }

  // Rewrite the journal records (simulating crash after DuckDB commit, before truncate).
  {
    MiniGitJournal leftover(journal_path_);
    std::string    append_error;
    for (const auto& record : journal->records()) {
      ASSERT_TRUE(leftover.Append(record, &append_error)) << append_error;
    }
  }

  // Recovery should skip the already-materialized prefix and truncate.
  std::string error;
  auto        result = recovery_->Recover(element_id_, journal_path_, &error);
  ASSERT_TRUE(result.accepted) << result.error;
  EXPECT_FALSE(result.materialized);

  auto               guard = storage_->GetDBController().GetConnectionGuard();
  auto               lock  = guard.Lock();
  CommitGraphService graph_service(guard.conn_);
  EXPECT_EQ(graph_service.CountCommitsForRoot(graph_->GetRootId()), 1u);

  MiniGitJournal reopened(journal_path_);
  ASSERT_TRUE(reopened.Load(&error)) << error;
  EXPECT_TRUE(reopened.records().empty());
}

TEST_F(EditorMiniGitJournalRecoveryTest, TruncateJournalFileReturnsTrueForEmptyPath) {
  EXPECT_TRUE(EditorMiniGitJournalRecovery::TruncateJournalFile({}));
}

TEST_F(EditorMiniGitJournalRecoveryTest, TruncateJournalFileReturnsTrueForNonExistent) {
  EXPECT_TRUE(EditorMiniGitJournalRecovery::TruncateJournalFile(
      std::filesystem::temp_directory_path() / "nonexistent_mini_git_recovery.wal"));
}

TEST_F(EditorMiniGitJournalRecoveryTest, TruncateJournalFileClearsExistingFile) {
  // Write a journal record first.
  auto                  journal = std::make_shared<MiniGitJournal>(journal_path_);
  MiniGitWorkingHistory history(graph_, journal);
  ASSERT_TRUE(history.AppendEdit(MakeExposurePayload(0.0f, 0.5f)).committed);
  ASSERT_TRUE(std::filesystem::exists(journal_path_));

  EXPECT_TRUE(EditorMiniGitJournalRecovery::TruncateJournalFile(journal_path_));

  MiniGitJournal reopened(journal_path_);
  std::string    error;
  ASSERT_TRUE(reopened.Load(&error)) << error;
  EXPECT_TRUE(reopened.records().empty());
}

TEST_F(EditorMiniGitJournalRecoveryTest, RecoveryWithNullErrorDoesNotCrash) {
  std::filesystem::remove(journal_path_);
  auto result = recovery_->Recover(element_id_, journal_path_, nullptr);
  EXPECT_TRUE(result.accepted);
}

}  // namespace alcedo
