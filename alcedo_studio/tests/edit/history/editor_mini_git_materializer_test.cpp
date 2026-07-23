//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "app/editor_mini_git_materializer.hpp"
#include "app/editor_save_checkpoint_coordinator.hpp"
#include "app/project_service.hpp"
#include "edit/history/commit_clock_test_access.hpp"
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

class EditorMiniGitMaterializerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    TimeProvider::Refresh();
    RegisterAllOperators();
    edit_history_test::CommitClockAccess::ResetGlobal(0);
    const auto stamp =
        std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const auto temp = std::filesystem::temp_directory_path();
    db_path_        = temp / ("mini_git_mat_" + stamp + ".db");
    meta_path_      = temp / ("mini_git_mat_" + stamp + ".json");
    journal_path_   = temp / ("mini_git_mat_" + stamp + ".mini-git.wal");
    std::error_code ec;
    std::filesystem::remove(db_path_, ec);
    std::filesystem::remove(meta_path_, ec);
    std::filesystem::remove(journal_path_, ec);

    project_ = std::make_unique<ProjectService>(db_path_, meta_path_);
    storage_ = project_->GetStorageService();
    {
      auto               guard = storage_->GetDBController().GetConnectionGuard();
      auto               lock  = guard.Lock();
      CommitGraphService graph_service(guard.conn_);
      graph_ = std::make_shared<CommitGraph>(
          graph_service.CreateEmptyPersisted(element_id_, "Default"));
    }
    materializer_ = std::make_unique<EditorMiniGitMaterializer>(storage_);
  }

  void TearDown() override {
    materializer_.reset();
    storage_.reset();
    project_.reset();
    std::error_code ec;
    std::filesystem::remove(db_path_, ec);
    std::filesystem::remove(meta_path_, ec);
    std::filesystem::remove(journal_path_, ec);
  }

  auto CaptureFromWorkingHistory(MiniGitWorkingHistory& history, MiniGitJournal& journal,
                                 float exposure) -> EditorMiniGitSaveCapture {
    EditorMiniGitSaveCapture capture;
    capture.element_id             = element_id_;
    capture.working_head           = history.working_head();
    capture.transaction_chain_hash = history.transaction_chain_hash();
    capture.journal_records        = journal.records();
    capture.journal_path           = journal_path_;
    capture.no_journal_changes     = capture.journal_records.empty();
    const auto serialized          = MakeEditorSerializedPipelineState(
        graph_->GetRootId(), capture.working_head, capture.transaction_chain_hash,
        nlohmann::json{{"exposure", exposure}});
    capture.materialization =
        graph_->CaptureMaterializationWithSerializedPipelineState(serialized);
    return capture;
  }

  sl_element_id_t                         element_id_ = 42;
  std::filesystem::path                   db_path_;
  std::filesystem::path                   meta_path_;
  std::filesystem::path                   journal_path_;
  std::unique_ptr<ProjectService>         project_;
  std::shared_ptr<StorageService>         storage_;
  std::shared_ptr<CommitGraph>            graph_;
  std::unique_ptr<EditorMiniGitMaterializer> materializer_;
};

TEST_F(EditorMiniGitMaterializerTest, EmptyJournalSucceedsWithoutMovingVersionHead) {
  auto journal = std::make_shared<MiniGitJournal>(journal_path_);
  MiniGitWorkingHistory history(graph_, journal);
  const auto prior_head = graph_->GetActiveVersionRef().head_commit_hash;
  auto       capture    = CaptureFromWorkingHistory(history, *journal, 0.0f);

  std::string error;
  const auto  result = materializer_->Materialize(capture, &error);
  ASSERT_TRUE(result.accepted) << error << " / " << result.error;
  ASSERT_TRUE(result.materialized);
  EXPECT_FALSE(result.head_moved);

  auto               guard = storage_->GetDBController().GetConnectionGuard();
  auto               lock  = guard.Lock();
  CommitGraphService graph_service(guard.conn_);
  auto               stored = graph_service.LoadGraph(element_id_);
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->GetActiveVersionRef().head_commit_hash, prior_head);
  EXPECT_EQ(stored->CommitCount(), 0u);
}

TEST_F(EditorMiniGitMaterializerTest, MaterializeWritesCommitVersionAndSerializedStateThenTruncates) {
  auto journal = std::make_shared<MiniGitJournal>(journal_path_);
  MiniGitWorkingHistory history(graph_, journal);
  ASSERT_TRUE(history.AppendEdit(MakeExposurePayload(0.0f, 1.25f)).committed);
  auto capture = CaptureFromWorkingHistory(history, *journal, 1.25f);
  ASSERT_FALSE(capture.journal_records.empty());
  ASSERT_TRUE(std::filesystem::exists(journal_path_));

  std::string error;
  const auto  result = materializer_->Materialize(capture, &error);
  ASSERT_TRUE(result.accepted) << error << " / " << result.error;
  ASSERT_TRUE(result.materialized);
  EXPECT_TRUE(result.head_moved);

  // Journal truncated after DuckDB success.
  MiniGitJournal reopened(journal_path_);
  ASSERT_TRUE(reopened.Load(&error)) << error;
  EXPECT_TRUE(reopened.records().empty());

  auto               guard = storage_->GetDBController().GetConnectionGuard();
  auto               lock  = guard.Lock();
  CommitGraphService graph_service(guard.conn_);
  auto               stored = graph_service.LoadGraph(element_id_);
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->CommitCount(), 1u);
  EXPECT_EQ(stored->GetActiveVersionRef().head_commit_hash, capture.working_head);
  EXPECT_EQ(stored->GetImageEditState().materialized_transaction_chain_hash,
            capture.transaction_chain_hash);
  ASSERT_TRUE(stored->GetImageEditState().serialized_pipeline_state.has_value());
  EXPECT_FLOAT_EQ(
      stored->GetImageEditState().serialized_pipeline_state->at("pipeline_params").at("exposure").get<float>(),
      1.25f);
}

TEST_F(EditorMiniGitMaterializerTest, FailureBeforeDuckDBCommitLeavesPriorHeadUnchanged) {
  auto journal = std::make_shared<MiniGitJournal>(journal_path_);
  MiniGitWorkingHistory history(graph_, journal);
  ASSERT_TRUE(history.AppendEdit(MakeExposurePayload(0.0f, 0.5f)).committed);
  auto capture = CaptureFromWorkingHistory(history, *journal, 0.5f);

  // Corrupt the capture head so journal fold validation rejects before DuckDB write.
  capture.working_head = Hash128{0xdead, 0xbeef};

  std::string error;
  const auto  result = materializer_->Materialize(capture, &error);
  EXPECT_FALSE(result.accepted);
  EXPECT_FALSE(result.materialized);

  auto               guard = storage_->GetDBController().GetConnectionGuard();
  auto               lock  = guard.Lock();
  CommitGraphService graph_service(guard.conn_);
  auto               stored = graph_service.LoadGraph(element_id_);
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->CommitCount(), 0u);
  EXPECT_FALSE(stored->GetActiveVersionRef().head_commit_hash.has_value());
}

TEST_F(EditorMiniGitMaterializerTest,
       CrashAfterDuckDBBeforeTruncateDoesNotReplayCommitTwice) {
  auto journal = std::make_shared<MiniGitJournal>(journal_path_);
  MiniGitWorkingHistory history(graph_, journal);
  ASSERT_TRUE(history.AppendEdit(MakeExposurePayload(0.0f, 0.75f)).committed);
  auto capture = CaptureFromWorkingHistory(history, *journal, 0.75f);

  std::string error;
  ASSERT_TRUE(materializer_->Materialize(capture, &error).accepted) << error;

  // Simulate leftover journal after DB commit by rewriting the captured records.
  {
    MiniGitJournal leftover(journal_path_);
    for (const auto& record : capture.journal_records) {
      ASSERT_TRUE(leftover.Append(record, &error)) << error;
    }
  }

  const auto recovered = materializer_->RecoverAndMaterialize(element_id_, journal_path_, &error);
  ASSERT_TRUE(recovered.accepted) << error << " / " << recovered.error;
  EXPECT_FALSE(recovered.head_moved);

  auto               guard = storage_->GetDBController().GetConnectionGuard();
  auto               lock  = guard.Lock();
  CommitGraphService graph_service(guard.conn_);
  EXPECT_EQ(graph_service.CountCommitsForRoot(graph_->GetRootId()), 1u);

  MiniGitJournal reopened(journal_path_);
  ASSERT_TRUE(reopened.Load(&error)) << error;
  EXPECT_TRUE(reopened.records().empty());
}

TEST_F(EditorMiniGitMaterializerTest, GlobalSaveLockSerializesConcurrentMaterializations) {
  EditorSaveCheckpointCoordinator coordinator;
  auto                            first = coordinator.TryAcquire(1);
  ASSERT_TRUE(first.owns_lock());
  auto second = coordinator.TryAcquire(2);
  EXPECT_FALSE(second.owns_lock());
  first.Release();
  second = coordinator.TryAcquire(2);
  EXPECT_TRUE(second.owns_lock());
}

}  // namespace alcedo
