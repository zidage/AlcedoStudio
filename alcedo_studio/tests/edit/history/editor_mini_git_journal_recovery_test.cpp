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
    // Isolated files from broken-recovery tests.
    for (const auto& entry : std::filesystem::directory_iterator(
             std::filesystem::temp_directory_path(), ec)) {
      if (!entry.is_regular_file()) continue;
      const auto name = entry.path().filename().string();
      if (name.find("mini_git_recovery_") != std::string::npos &&
          name.find(".isolated.") != std::string::npos) {
        std::filesystem::remove(entry.path(), ec);
      }
    }
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

/// Normal save already persisted history; leftover WAL is fully covered and must
/// clear without re-writing DuckDB (no WAL-to-DB fold).
TEST_F(EditorMiniGitJournalRecoveryTest, FullyCoveredWalClearsWithoutRewritingDb) {
  auto                  journal = std::make_shared<MiniGitJournal>(journal_path_);
  MiniGitWorkingHistory history(graph_, journal);
  ASSERT_TRUE(history.AppendEdit(MakeExposurePayload(0.0f, 0.75f)).committed);
  ASSERT_FALSE(journal->records().empty());

  // Persist the unique history snapshot via CommitWriter (normal save path).
  {
    auto materialization = graph_->CaptureMaterializationWithSerializedPipelineState(
        graph_->GetImageEditState().serialized_pipeline_state);
    EditorMiniGitCommitWriter writer(storage_);
    std::string               write_error;
    auto                      write_result = writer.Write(materialization, &write_error);
    ASSERT_TRUE(write_result.accepted) << write_result.error;
  }

  // Journal intentionally left intact (crash after DB, before WAL clear).
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

/// Contiguous missing suffix is accepted and left for live attach (not folded here).
TEST_F(EditorMiniGitJournalRecoveryTest, ContiguousMissingSuffixLeavesWalForLiveAttach) {
  auto                  journal = std::make_shared<MiniGitJournal>(journal_path_);
  MiniGitWorkingHistory history(graph_, journal);
  ASSERT_TRUE(history.AppendEdit(MakeExposurePayload(0.0f, 0.5f)).committed);
  ASSERT_FALSE(journal->records().empty());

  // DB still at empty head — WAL is a contiguous extension.
  std::string error;
  auto        result = recovery_->Recover(element_id_, journal_path_, &error);
  ASSERT_TRUE(result.accepted) << result.error;
  EXPECT_FALSE(result.materialized);

  // WAL must remain for EnsureWorkingState / live pipeline apply.
  MiniGitJournal reopened(journal_path_);
  ASSERT_TRUE(reopened.Load(&error)) << error;
  EXPECT_EQ(reopened.records().size(), 1u);

  // DuckDB must be unchanged (no fold).
  auto               guard = storage_->GetDBController().GetConnectionGuard();
  auto               lock  = guard.Lock();
  CommitGraphService graph_service(guard.conn_);
  EXPECT_EQ(graph_service.CountCommitsForRoot(graph_->GetRootId()), 0u);
}

TEST_F(EditorMiniGitJournalRecoveryTest, TruncateJournalFileReturnsTrueForEmptyPath) {
  EXPECT_TRUE(EditorMiniGitJournalRecovery::TruncateJournalFile({}));
}

TEST_F(EditorMiniGitJournalRecoveryTest, TruncateJournalFileReturnsTrueForNonExistent) {
  EXPECT_TRUE(EditorMiniGitJournalRecovery::TruncateJournalFile(
      std::filesystem::temp_directory_path() / "nonexistent_mini_git_recovery.wal"));
}

TEST_F(EditorMiniGitJournalRecoveryTest, TruncateJournalFileClearsExistingFile) {
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

/// Broken parent chain isolates WAL and leaves DuckDB unchanged.
TEST_F(EditorMiniGitJournalRecoveryTest, BrokenParentIsolatesWalAndWritesNothing) {
  auto                  journal = std::make_shared<MiniGitJournal>(journal_path_);
  MiniGitWorkingHistory history(graph_, journal);
  ASSERT_TRUE(history.AppendEdit(MakeExposurePayload(0.0f, 0.5f)).committed);
  ASSERT_FALSE(journal->records().empty());

  auto records = journal->records();
  records.front().expected_source_head = Hash128{0xbad, 0xc0de};
  records.front().expected_source_chain_hash = Hash128{0xdead, 0xbeef};

  // Rewrite journal with the broken record.
  ASSERT_TRUE(EditorMiniGitJournalRecovery::TruncateJournalFile(journal_path_));
  MiniGitJournal bad_journal(journal_path_);
  for (const auto& rec : records) {
    std::string append_error;
    ASSERT_TRUE(bad_journal.Append(rec, &append_error)) << append_error;
  }

  std::string error;
  auto        result = recovery_->Recover(element_id_, journal_path_, &error);
  EXPECT_FALSE(result.accepted);
  EXPECT_FALSE(result.error.empty());

  // Original path should be gone (isolated) or empty of usable records.
  EXPECT_FALSE(std::filesystem::exists(journal_path_));

  auto               guard = storage_->GetDBController().GetConnectionGuard();
  auto               lock  = guard.Lock();
  CommitGraphService graph_service(guard.conn_);
  EXPECT_EQ(graph_service.CountCommitsForRoot(graph_->GetRootId()), 0u);
}

/// Normal save clears WAL without materializing from WAL records (write path
/// uses unique history capture only).
TEST_F(EditorMiniGitJournalRecoveryTest, NormalSaveCaptureDoesNotRequireJournalFold) {
  auto                  journal = std::make_shared<MiniGitJournal>(journal_path_);
  MiniGitWorkingHistory history(graph_, journal);
  ASSERT_TRUE(history.AppendEdit(MakeExposurePayload(0.0f, 1.0f)).committed);

  auto materialization = graph_->CaptureMaterializationWithSerializedPipelineState(
      nlohmann::json{{"state_format_version", 1},
                     {"pipeline_params", nlohmann::json{{"exposure", 1.0f}}}});
  EditorMiniGitCommitWriter writer(storage_);
  std::string               write_error;
  auto                      write_result = writer.Write(materialization, &write_error);
  ASSERT_TRUE(write_result.accepted) << write_result.error;

  ASSERT_TRUE(EditorMiniGitJournalRecovery::TruncateJournalFile(journal_path_));

  auto               guard = storage_->GetDBController().GetConnectionGuard();
  auto               lock  = guard.Lock();
  CommitGraphService graph_service(guard.conn_);
  EXPECT_EQ(graph_service.CountCommitsForRoot(graph_->GetRootId()), 1u);
}

}  // namespace alcedo
