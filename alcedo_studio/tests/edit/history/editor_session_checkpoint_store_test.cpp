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
    store_ = std::make_unique<EditorSessionCheckpointStore>();
    store_->SetServices(EditorSessionCheckpointStore::Services{
        [this] { return storage_; }, [this](sl_element_id_t) { return journal_path_; }});
  }

  void TearDown() override {
    store_.reset();
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
    alcedo::EditorMiniGitSaveCapture capture;
    capture.element_id             = element_id_;
    capture.working_head           = history.working_head();
    capture.transaction_chain_hash = history.transaction_chain_hash();
    capture.journal_records        = journal->records();
    capture.journal_path           = journal_path_;
    const auto serialized          = alcedo::MakeEditorSerializedPipelineState(
        graph_->GetRootId(), capture.working_head, capture.transaction_chain_hash,
        nlohmann::json{{"exposure", 1.25f}});
    capture.materialization = graph_->CaptureMaterializationWithSerializedPipelineState(serialized);
    return capture;
  }

  static constexpr sl_element_id_t              element_id_ = 42;
  std::filesystem::path                         db_path_;
  std::filesystem::path                         meta_path_;
  std::filesystem::path                         journal_path_;
  std::unique_ptr<alcedo::ProjectService>       project_;
  std::shared_ptr<alcedo::StorageService>       storage_;
  std::shared_ptr<alcedo::CommitGraph>          graph_;
  std::unique_ptr<EditorSessionCheckpointStore> store_;
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

}  // namespace
}  // namespace alcedo::ui
