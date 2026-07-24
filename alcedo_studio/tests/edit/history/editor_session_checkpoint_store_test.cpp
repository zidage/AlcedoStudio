//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_session_checkpoint_store.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>

#include "app/editor_mini_git_materializer.hpp"
#include "app/editor_save_checkpoint_coordinator.hpp"
#include "app/editor_session_bootstrap.hpp"
#include "app/project_service.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "storage/service/sleeve/edit_history/commit_graph_service.hpp"

namespace alcedo::ui {
namespace {

auto MakeExposurePayload(float before, float after) -> alcedo::OrdinaryEditPayload {
  alcedo::OrdinaryEditPayload payload;
  payload.operator_type  = alcedo::OperatorType::EXPOSURE;
  payload.stage_name     = alcedo::PipelineStageName::Basic_Adjustment;
  payload.field_name     = "$operator_params";
  payload.before_value   = nlohmann::json{{"exposure", before}};
  payload.after_value    = nlohmann::json{{"exposure", after}};
  payload.before_enabled = true;
  payload.after_enabled  = true;
  return payload;
}

class EditorSessionCheckpointStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RegisterAllOperators();
    const auto stamp =
        std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const auto temp = std::filesystem::temp_directory_path();
    db_path_        = temp / ("session_checkpoint_" + stamp + ".db");
    meta_path_      = temp / ("session_checkpoint_" + stamp + ".json");
    journal_path_   = temp / ("session_checkpoint_" + stamp + ".mini-git.wal");
    project_        = std::make_unique<alcedo::ProjectService>(db_path_, meta_path_);
    storage_        = project_->GetStorageService();
    auto                       guard = storage_->GetDBController().GetConnectionGuard();
    auto                       lock  = guard.Lock();
    alcedo::CommitGraphService graph_service(guard.conn_);
    graph_ = std::make_shared<alcedo::CommitGraph>(
        graph_service.CreateEmptyPersisted(element_id_, "Default"));
    save_coordinator_ = std::make_shared<alcedo::EditorSaveCheckpointCoordinator>();
    store_            = std::make_unique<EditorSessionCheckpointStore>();
    store_->SetServices(EditorSessionCheckpointStore::Services{
        [this] { return storage_; }, [this](sl_element_id_t) { return journal_path_; },
        save_coordinator_});
  }

  void TearDown() override {
    store_.reset();
    if (save_coordinator_) {
      save_coordinator_->Shutdown();
    }
    save_coordinator_.reset();
    storage_.reset();
    project_.reset();
    std::error_code ec;
    std::filesystem::remove(db_path_, ec);
    std::filesystem::remove(meta_path_, ec);
    std::filesystem::remove(journal_path_, ec);
  }

  auto CaptureEdit() -> alcedo::EditorMiniGitSaveCapture {
    auto                          journal = std::make_shared<alcedo::MiniGitJournal>(journal_path_);
    alcedo::MiniGitWorkingHistory history(graph_, journal);
    if (!history.AppendEdit(MakeExposurePayload(0.0f, 1.25f)).committed) {
      throw std::runtime_error("failed to append test capture");
    }
    const auto                       snapshot = journal->Snapshot();
    alcedo::EditorMiniGitSaveCapture capture;
    capture.element_id             = element_id_;
    capture.version_id             = graph_->GetActiveVersionId();
    capture.root_id                = graph_->GetRootId();
    capture.working_head           = history.working_head();
    capture.transaction_chain_hash = history.transaction_chain_hash();
    capture.journal_records        = snapshot.records;
    capture.journal_path           = journal_path_;
    capture.first_journal_sequence = snapshot.first_sequence;
    capture.last_journal_sequence  = snapshot.last_sequence;
    const auto serialized          = alcedo::MakeEditorSerializedPipelineState(
        graph_->GetRootId(), capture.working_head, capture.transaction_chain_hash,
        nlohmann::json{{"exposure", 1.25f}});
    capture.materialization = graph_->CaptureMaterializationWithSerializedPipelineState(serialized);
    return capture;
  }

  static constexpr sl_element_id_t                                 element_id_ = 42;
  std::filesystem::path                                            db_path_;
  std::filesystem::path                                            meta_path_;
  std::filesystem::path                                            journal_path_;
  std::unique_ptr<alcedo::ProjectService>                          project_;
  std::shared_ptr<alcedo::StorageService>                          storage_;
  std::shared_ptr<alcedo::CommitGraph>                             graph_;
  std::shared_ptr<alcedo::EditorSaveCheckpointCoordinator>         save_coordinator_;
  std::unique_ptr<EditorSessionCheckpointStore>                    store_;
};

TEST_F(EditorSessionCheckpointStoreTest, MaterializePersistsCaptureBeforeTruncatingJournal) {
  const auto  capture = CaptureEdit();
  std::string error;
  const auto  result = store_->Materialize(
      std::make_shared<const alcedo::EditorMiniGitSaveCapture>(capture), &error);
  ASSERT_TRUE(result.accepted) << error << " / " << result.error;
  ASSERT_TRUE(result.materialized);

  alcedo::MiniGitJournal reopened(journal_path_);
  ASSERT_TRUE(reopened.Load(&error)) << error;
  EXPECT_TRUE(reopened.records().empty());
  auto                       guard = storage_->GetDBController().GetConnectionGuard();
  auto                       lock  = guard.Lock();
  alcedo::CommitGraphService graph_service(guard.conn_);
  const auto                 stored = graph_service.LoadGraph(element_id_);
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->CommitCount(), 1u);
  EXPECT_EQ(stored->GetActiveVersionRef().head_commit_hash, capture.working_head);
}

/// Phase 4A: immutable capture ownership travels as a function argument into the
/// store; no element-ID side map or TakeSaveCapture is involved.
TEST_F(EditorSessionCheckpointStoreTest, ProductionCaptureValueReachesCheckpointStoreWithoutSideMap) {
  const auto capture = CaptureEdit();
  auto owned = std::make_shared<const alcedo::EditorMiniGitSaveCapture>(capture);
  const auto* raw = owned.get();
  const auto  head = owned->working_head;
  const auto  last = *owned->last_journal_sequence;

  std::string error;
  const auto  result = store_->Materialize(owned, &error);
  ASSERT_TRUE(result.accepted) << error << " / " << result.error;
  ASSERT_TRUE(result.materialized);
  // Caller still owns the shared capture after Materialize (store does not
  // take exclusive ownership via a side channel).
  EXPECT_EQ(owned.get(), raw);
  EXPECT_EQ(owned->working_head, head);
  EXPECT_EQ(*owned->last_journal_sequence, last);
  EXPECT_EQ(owned.use_count(), 1);

  auto                       guard = storage_->GetDBController().GetConnectionGuard();
  auto                       lock  = guard.Lock();
  alcedo::CommitGraphService graph_service(guard.conn_);
  const auto                 stored = graph_service.LoadGraph(element_id_);
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->GetActiveVersionRef().head_commit_hash, head);
}

TEST_F(EditorSessionCheckpointStoreTest, RecoveryUsesConfiguredImageJournalPathAndTruncatesPrefix) {
  const auto  capture = CaptureEdit();
  std::string error;
  ASSERT_TRUE(
      store_->Materialize(std::make_shared<const alcedo::EditorMiniGitSaveCapture>(capture), &error)
          .materialized)
      << error;
  alcedo::MiniGitJournal leftover(journal_path_);
  for (const auto& record : capture.journal_records) {
    ASSERT_TRUE(leftover.Append(record, &error)) << error;
  }
  const auto result = store_->RecoverAndMaterialize(element_id_, 1, &error);
  EXPECT_TRUE(result.accepted) << error << " / " << result.error;
  EXPECT_FALSE(result.materialized);
  alcedo::MiniGitJournal reopened(journal_path_);
  ASSERT_TRUE(reopened.Load(&error)) << error;
  EXPECT_TRUE(reopened.records().empty());
}

/// Phase 4A: null capture and missing storage fail; never report a silent success.
TEST_F(EditorSessionCheckpointStoreTest, ConfiguredProjectWithoutHistoryStoreOrStorageFails) {
  std::string error;
  const auto  null_result = store_->Materialize(nullptr, &error);
  EXPECT_FALSE(null_result.accepted);
  EXPECT_FALSE(null_result.materialized);
  EXPECT_FALSE(error.empty());

  EditorSessionCheckpointStore unconfigured;
  error.clear();
  const auto capture = CaptureEdit();
  const auto no_storage =
      unconfigured.Materialize(std::make_shared<const alcedo::EditorMiniGitSaveCapture>(capture),
                               &error);
  EXPECT_FALSE(no_storage.accepted);
  EXPECT_FALSE(no_storage.materialized);
  EXPECT_NE(error.find("unavailable"), std::string::npos);
}

/// Phase 4A: store materializes only through Mini-Git (commit graph + truncate).
/// The legacy transaction-array materializer is never used on this path.
TEST_F(EditorSessionCheckpointStoreTest, MiniGitCheckpointDoesNotInvokeLegacyMaterializer) {
  const auto  capture = CaptureEdit();
  std::string error;
  const auto  result = store_->Materialize(
      std::make_shared<const alcedo::EditorMiniGitSaveCapture>(capture), &error);
  ASSERT_TRUE(result.accepted) << error << " / " << result.error;
  ASSERT_TRUE(result.materialized);

  // Mini-Git durable state: one content-addressed commit, active Version head,
  // and truncated journal. Legacy transaction-array materialization does not
  // write CommitGraph commit objects.
  auto                       guard = storage_->GetDBController().GetConnectionGuard();
  auto                       lock  = guard.Lock();
  alcedo::CommitGraphService graph_service(guard.conn_);
  const auto                 stored = graph_service.LoadGraph(element_id_);
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->CommitCount(), 1u);
  EXPECT_EQ(stored->GetActiveVersionRef().head_commit_hash, capture.working_head);
  EXPECT_EQ(stored->GetImageEditState().materialized_head_commit_hash, capture.working_head);

  alcedo::MiniGitJournal reopened(journal_path_);
  ASSERT_TRUE(reopened.Load(&error)) << error;
  EXPECT_TRUE(reopened.records().empty());
}

/// Phase 4A: capture failure / rejected materialize writes nothing and keeps journal bytes.
TEST_F(EditorSessionCheckpointStoreTest, CaptureFailureWritesNothingAndLeavesJournalBytes) {
  auto        capture = CaptureEdit();
  ASSERT_TRUE(capture.has_journal_range());
  const auto preserved_size = capture.journal_records.size();
  const auto preserved_last = *capture.last_journal_sequence;

  // Corrupt the captured head so fold validation rejects before DuckDB commit.
  capture.working_head = alcedo::Hash128{0xdead, 0xbeef};
  std::string error;
  const auto  result = store_->Materialize(
      std::make_shared<const alcedo::EditorMiniGitSaveCapture>(capture), &error);
  EXPECT_FALSE(result.accepted);
  EXPECT_FALSE(result.materialized);

  alcedo::MiniGitJournal remaining(journal_path_);
  ASSERT_TRUE(remaining.Load(&error)) << error;
  EXPECT_EQ(remaining.records().size(), preserved_size);
  EXPECT_EQ(remaining.records().back().sequence, preserved_last);

  auto                       guard = storage_->GetDBController().GetConnectionGuard();
  auto                       lock  = guard.Lock();
  alcedo::CommitGraphService graph_service(guard.conn_);
  const auto                 stored = graph_service.LoadGraph(element_id_);
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->CommitCount(), 0u);
}

}  // namespace
}  // namespace alcedo::ui
