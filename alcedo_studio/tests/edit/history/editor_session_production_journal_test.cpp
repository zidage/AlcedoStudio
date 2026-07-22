//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "app/editor_adjustment_pipeline.hpp"
#include "app/editor_history_materializer.hpp"
#include "app/editor_session_bootstrap.hpp"
#include "app/project_service.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/edit_history.hpp"
#include "edit/history/edit_transaction.hpp"
#include "edit/history/editor_journal_writer.hpp"
#include "edit/history/editor_transaction_journal.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "edit/history/version.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"
#include "type/hash_type.hpp"
#include "type/type.hpp"
#include "ui/alcedo_main/album_backend/editor_session_production.hpp"
#include "utils/clock/time_provider.hpp"

namespace alcedo::ui {
namespace {

auto MakeExposureTransaction(float exposure, tx_id_t id) -> alcedo::EditTransaction {
  alcedo::EditTransaction transaction{alcedo::TransactionType::_EDIT,
                                      alcedo::OperatorType::EXPOSURE,
                                      alcedo::PipelineStageName::Basic_Adjustment,
                                      nlohmann::json{{"exposure", 0.0f}},
                                      nlohmann::json{{"exposure", exposure}},
                                      true,
                                      true};
  transaction.SetTransactionID(id);
  transaction.GenerateTransactionHash();
  return transaction;
}

auto MakePipelineParams(float exposure) -> nlohmann::json {
  alcedo::CPUPipelineExecutor exec;
  auto&                       stage = exec.GetStage(alcedo::PipelineStageName::Basic_Adjustment);
  auto                        op    = stage.GetOperator(alcedo::OperatorType::EXPOSURE);
  if (op.has_value() && op.value() != nullptr) {
    op.value()->op_->SetParams(nlohmann::json{{"exposure", exposure}});
    op.value()->enable_ = true;
  }
  auto params                    = exec.ExportPipelineParams();
  params["alcedo_test_exposure"] = exposure;
  return params;
}

auto MakeMiniGitPipelineGuard(sl_element_id_t element_id)
    -> std::shared_ptr<alcedo::PipelineGuard> {
  auto guard       = std::make_shared<alcedo::PipelineGuard>();
  guard->id_       = element_id;
  guard->pipeline_ = std::make_shared<alcedo::CPUPipelineExecutor>();
  guard->commit_graph_ =
      std::make_shared<alcedo::CommitGraph>(alcedo::CommitGraph::CreateEmpty(element_id));
  guard->root_id_                  = guard->commit_graph_->GetRootId();
  guard->working_head_commit_hash_ = std::nullopt;
  guard->transaction_chain_hash_   = alcedo::ComputeRootChainHash(guard->root_id_);
  return guard;
}

class EditorSessionProductionJournalPortTest : public ::testing::Test {
 protected:
  void SetUp() override {
    alcedo::TimeProvider::Refresh();
    alcedo::RegisterAllOperators();
    const auto stamp =
        std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const auto temp = std::filesystem::temp_directory_path();
    db_path_        = temp / ("prod_journal_" + stamp + ".db");
    meta_path_      = temp / ("prod_journal_" + stamp + ".json");
    journal_dir_    = temp / ("prod_journal_" + stamp + "_journal");
    std::error_code ec;
    std::filesystem::remove(db_path_, ec);
    std::filesystem::remove(meta_path_, ec);
    std::filesystem::remove_all(journal_dir_, ec);
    std::filesystem::create_directories(journal_dir_, ec);
    journal_path_ = journal_dir_ / "image-42.wal";

    project_      = std::make_unique<alcedo::ProjectService>(db_path_, meta_path_);
    storage_      = project_->GetStorageService();
    // Seed a durable history for the element so the port's load_history resolver
    // returns a non-null EditHistory (recovery/materialize layer on top of it).
    auto history  = std::make_shared<alcedo::EditHistory>(file_id_);
    history->SetImportPipelineParams(MakePipelineParams(0.0f));
    storage_->GetElementController().UpdateEditHistoryByFileId(file_id_, history);

    port_ = std::make_shared<alcedo::ui::EditorSessionProductionJournalPort>(MakeServices());
  }

  void TearDown() override {
    port_.reset();
    project_.reset();
    std::error_code ec;
    std::filesystem::remove(db_path_, ec);
    std::filesystem::remove(meta_path_, ec);
    std::filesystem::remove_all(journal_dir_, ec);
  }

  auto MakeServices() -> alcedo::ui::EditorSessionProductionServices {
    alcedo::ui::EditorSessionProductionServices services;
    services.storage_service = [this]() { return storage_; };
    services.load_history    = [this](sl_element_id_t id) -> std::shared_ptr<alcedo::EditHistory> {
      try {
        return storage_->GetElementController().GetEditHistoryByFileId(id);
      } catch (...) {
        return nullptr;
      }
    };
    services.load_pipeline =
        [this](sl_element_id_t id) -> std::shared_ptr<alcedo::CPUPipelineExecutor> {
      try {
        return storage_->GetElementController().GetPipelineByElementId(id);
      } catch (...) {
        return nullptr;
      }
    };
    services.journal_path = [this](sl_element_id_t id) {
      return journal_dir_ / ("image-" + std::to_string(static_cast<std::uint64_t>(id)) + ".wal");
    };
    services.invalidate_thumbnail = [](sl_element_id_t) {};
    return services;
  }

  static constexpr sl_element_id_t                                file_id_ = 42;

  std::filesystem::path                                           db_path_;
  std::filesystem::path                                           meta_path_;
  std::filesystem::path                                           journal_dir_;
  std::filesystem::path                                           journal_path_;
  std::unique_ptr<alcedo::ProjectService>                         project_;
  std::shared_ptr<alcedo::StorageService>                         storage_;
  std::shared_ptr<alcedo::ui::EditorSessionProductionJournalPort> port_;
};

TEST_F(EditorSessionProductionJournalPortTest, RecordEditAndCommitDurablyAppendTheEdit) {
  std::string error;
  EXPECT_TRUE(port_->RecordEdit(file_id_, 1, MakeExposureTransaction(1.5f, 1), &error)) << error;
  const auto committed = port_->CommitJournal(file_id_, 1, &error);
  ASSERT_TRUE(committed.accepted) << committed.error;
  EXPECT_TRUE(committed.durable);
  EXPECT_GT(committed.durable_operation_sequence, 0u);
}

TEST_F(EditorSessionProductionJournalPortTest, PipelinePortReleaseReturnsServicePin) {
  auto pipeline_service     = std::make_shared<alcedo::PipelineMgmtService>(storage_);
  auto services             = MakeServices();
  services.pipeline_service = [pipeline_service]() { return pipeline_service; };

  alcedo::ui::EditorSessionProductionPipelinePort pipeline_port;
  pipeline_port.SetServices(std::move(services));
  std::string error;
  const auto  handle = pipeline_port.Acquire(file_id_, &error);
  ASSERT_TRUE(handle.valid);

  auto loaded = pipeline_port.EnsureLoaded(file_id_, &error);
  ASSERT_NE(loaded, nullptr) << error;
  EXPECT_EQ(loaded->pin_count_, 1u);

  pipeline_port.Release(handle);
  EXPECT_EQ(loaded->pin_count_, 0u);
  EXPECT_EQ(pipeline_port.CurrentGuard(file_id_), nullptr);
}

TEST_F(EditorSessionProductionJournalPortTest,
       QmlPreviewSamplesAndSettledReleaseCreateOneMiniGitCommit) {
  auto guard                          = MakeMiniGitPipelineGuard(file_id_);
  auto services                       = MakeServices();
  services.load_editor_pipeline_guard = [guard](sl_element_id_t) { return guard; };
  const auto mini_git_path            = journal_dir_ / "image-42.mini-git.wal";
  services.mini_git_journal_path      = [mini_git_path](sl_element_id_t) { return mini_git_path; };

  auto pipeline_port                  = std::make_shared<EditorSessionProductionPipelinePort>();
  pipeline_port->SetServices(services);
  EditorSessionProductionHistoryPort history_port;
  history_port.SetServices(services);
  history_port.SetPipelinePort(pipeline_port);

  std::string error;
  const auto  handle = history_port.Acquire(file_id_, &error);
  ASSERT_TRUE(handle.valid) << error;
  const alcedo::EditorAdjustmentPatch first_preview{"exposure", R"({"exposure":0.25})", false};
  const alcedo::EditorAdjustmentPatch second_preview{"exposure", R"({"exposure":0.5})", false};
  const alcedo::EditorAdjustmentPatch settled{"exposure", R"({"exposure":0.75})", true};
  ASSERT_TRUE(history_port.CaptureAdjustmentBeforePreview(handle, first_preview, &error)) << error;
  ASSERT_TRUE(history_port.CaptureAdjustmentBeforePreview(handle, second_preview, &error)) << error;
  ASSERT_TRUE(history_port.CaptureAdjustmentBeforePreview(handle, settled, &error)) << error;
  ASSERT_TRUE(history_port.CommitAdjustment(handle, settled, &error)) << error;

  ASSERT_EQ(guard->commit_graph_->CommitCount(), 1u);
  ASSERT_TRUE(guard->working_head_commit_hash_.has_value());
  EXPECT_EQ(guard->working_head_commit_hash_,
            guard->commit_graph_->GetActiveVersionRef().head_commit_hash);
  EXPECT_EQ(guard->transaction_chain_hash_,
            guard->commit_graph_->ChainHashForHead(guard->working_head_commit_hash_));

  alcedo::MiniGitJournal reopened(mini_git_path);
  ASSERT_TRUE(reopened.Load(&error)) << error;
  ASSERT_EQ(reopened.records().size(), 1u);
  EXPECT_EQ(reopened.records().front().kind, alcedo::MiniGitJournalRecordKind::kEditCommit);

  ASSERT_TRUE(history_port.Undo(handle, &error)) << error;
  EXPECT_FALSE(guard->working_head_commit_hash_.has_value());
  ASSERT_TRUE(history_port.Redo(handle, &error)) << error;
  EXPECT_TRUE(guard->working_head_commit_hash_.has_value());
}

TEST_F(EditorSessionProductionJournalPortTest,
       QmlSessionMultipleInteractiveSamplesCommitOneMiniGitEdit) {
  auto guard                          = MakeMiniGitPipelineGuard(file_id_);
  auto services                       = MakeServices();
  services.load_editor_pipeline_guard = [guard](sl_element_id_t) { return guard; };
  const auto mini_git_path            = journal_dir_ / "image-42.session.mini-git.wal";
  services.mini_git_journal_path      = [mini_git_path](sl_element_id_t) { return mini_git_path; };

  auto pipeline_port                  = std::make_shared<EditorSessionProductionPipelinePort>();
  pipeline_port->SetServices(services);
  auto history_port = std::make_shared<EditorSessionProductionHistoryPort>();
  history_port->SetServices(services);
  history_port->SetPipelinePort(pipeline_port);
  auto scheduler = std::make_shared<alcedo::EditorSessionBootstrapSchedulerPort>();
  auto runtime   = alcedo::EditorSessionRuntime::CreateWithPorts(
      pipeline_port, history_port, std::make_shared<alcedo::EditorSessionBootstrapTaskPort>(),
      std::make_shared<alcedo::EditorSessionBootstrapJournalPort>(), scheduler);

  runtime->service->SetPresentationSinkId(1);
  runtime->service->SetPresentationSize(640, 480);
  runtime->service->Open(file_id_, 7);
  ASSERT_FALSE(scheduler->scheduled().empty());
  const auto request_id = scheduler->scheduled().front().request_id;
  runtime->coordinator->NotifySchedulerCompleted(request_id, true);
  runtime->coordinator->NotifyFrameSubmitted(request_id);
  runtime->coordinator->NotifyFramePresented(request_id);
  ASSERT_EQ(runtime->service->state(), alcedo::EditorSessionState::Interactive);

  for (int sample = 1; sample <= 20; ++sample) {
    alcedo::EditorAdjustmentPatch preview{
        "exposure", std::string{"{\"exposure\":"} + std::to_string(sample / 20.0) + "}", false};
    EXPECT_NE(runtime->service->Patch(std::move(preview)).kind,
              alcedo::EditorSessionResultKind::Rejected);
  }
  alcedo::EditorAdjustmentPatch settled{"exposure", R"({"exposure":1.0})", true};
  EXPECT_NE(runtime->service->CommitAdjustment(std::move(settled)).kind,
            alcedo::EditorSessionResultKind::Rejected);

  EXPECT_EQ(guard->commit_graph_->CommitCount(), 1u);
  alcedo::MiniGitJournal reopened(mini_git_path);
  std::string            error;
  ASSERT_TRUE(reopened.Load(&error)) << error;
  ASSERT_EQ(reopened.records().size(), 1u);
  EXPECT_EQ(reopened.records().front().kind, alcedo::MiniGitJournalRecordKind::kEditCommit);
}

TEST_F(EditorSessionProductionJournalPortTest,
       MiniGitJournalFailureLeavesQmlWorkingHeadAndChainAtRoot) {
  auto guard                          = MakeMiniGitPipelineGuard(file_id_);
  auto services                       = MakeServices();
  services.load_editor_pipeline_guard = [guard](sl_element_id_t) { return guard; };
  const auto blocked_parent           = journal_dir_ / "not-a-directory";
  {
    std::ofstream blocker(blocked_parent, std::ios::binary);
    ASSERT_TRUE(blocker.is_open());
    blocker << "block";
  }
  services.mini_git_journal_path = [blocked_parent](sl_element_id_t) {
    return blocked_parent / "image-42.mini-git.wal";
  };

  auto pipeline_port = std::make_shared<EditorSessionProductionPipelinePort>();
  pipeline_port->SetServices(services);
  EditorSessionProductionHistoryPort history_port;
  history_port.SetServices(services);
  history_port.SetPipelinePort(pipeline_port);

  std::string error;
  const auto  handle = history_port.Acquire(file_id_, &error);
  ASSERT_TRUE(handle.valid) << error;
  const alcedo::EditorAdjustmentPatch settled{"exposure", R"({"exposure":1.0})", true};
  ASSERT_TRUE(history_port.CaptureAdjustmentBeforePreview(handle, settled, &error)) << error;
  EXPECT_FALSE(history_port.CommitAdjustment(handle, settled, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(guard->commit_graph_->CommitCount(), 0u);
  EXPECT_FALSE(guard->working_head_commit_hash_.has_value());
  EXPECT_EQ(guard->transaction_chain_hash_, alcedo::ComputeRootChainHash(guard->root_id_));
}

TEST_F(EditorSessionProductionJournalPortTest,
       ReopeningQmlHistoryReplaysMiniGitJournalIntoPipelineAndHead) {
  const auto mini_git_path            = journal_dir_ / "image-42.recovery.mini-git.wal";
  auto       services                 = MakeServices();
  services.mini_git_journal_path      = [mini_git_path](sl_element_id_t) { return mini_git_path; };

  auto       first_guard              = MakeMiniGitPipelineGuard(file_id_);
  const auto recovery_base            = *first_guard->commit_graph_;
  services.load_editor_pipeline_guard = [first_guard](sl_element_id_t) { return first_guard; };
  auto first_pipeline_port            = std::make_shared<EditorSessionProductionPipelinePort>();
  first_pipeline_port->SetServices(services);
  {
    EditorSessionProductionHistoryPort history_port;
    history_port.SetServices(services);
    history_port.SetPipelinePort(first_pipeline_port);
    std::string error;
    const auto  handle = history_port.Acquire(file_id_, &error);
    ASSERT_TRUE(handle.valid) << error;
    const alcedo::EditorAdjustmentPatch settled{"exposure", R"({"exposure":1.25})", true};
    ASSERT_TRUE(history_port.CaptureAdjustmentBeforePreview(handle, settled, &error)) << error;
    ASSERT_TRUE(history_port.CommitAdjustment(handle, settled, &error)) << error;
  }

  auto recovered_guard           = MakeMiniGitPipelineGuard(file_id_);
  recovered_guard->commit_graph_ = std::make_shared<alcedo::CommitGraph>(recovery_base);
  recovered_guard->root_id_      = recovery_base.GetRootId();
  recovered_guard->transaction_chain_hash_ =
      alcedo::ComputeRootChainHash(recovered_guard->root_id_);
  services.load_editor_pipeline_guard = [recovered_guard](sl_element_id_t) {
    return recovered_guard;
  };
  auto recovered_pipeline_port = std::make_shared<EditorSessionProductionPipelinePort>();
  recovered_pipeline_port->SetServices(services);
  EditorSessionProductionHistoryPort recovered_history;
  recovered_history.SetServices(services);
  recovered_history.SetPipelinePort(recovered_pipeline_port);
  std::string error;
  const auto  recovered_handle = recovered_history.Acquire(file_id_, &error);
  ASSERT_TRUE(recovered_handle.valid) << error;

  ASSERT_EQ(recovered_guard->commit_graph_->CommitCount(), 1u);
  EXPECT_TRUE(recovered_guard->working_head_commit_hash_.has_value());
  alcedo::EditorAdjustmentOperatorState exposure;
  ASSERT_TRUE(alcedo::ReadEditorAdjustmentOperatorState(*recovered_guard->pipeline_, "exposure",
                                                        &exposure, &error))
      << error;
  EXPECT_FLOAT_EQ(exposure.params.at("exposure").get<float>(), 1.25f);
}

TEST_F(EditorSessionProductionJournalPortTest,
       SessionServiceQueuesEditCursorMoveAndTimelineRewriteInProductionWriter) {
  auto scheduler = std::make_shared<alcedo::EditorSessionBootstrapSchedulerPort>();
  auto runtime   = alcedo::EditorSessionRuntime::CreateWithPorts(
      std::make_shared<alcedo::EditorSessionBootstrapPipelinePort>(),
      std::make_shared<alcedo::EditorSessionBootstrapHistoryPort>(),
      std::make_shared<alcedo::EditorSessionBootstrapTaskPort>(), port_, scheduler);
  runtime->service->SetPresentationSinkId(1);
  runtime->service->SetPresentationSize(640, 480);
  runtime->service->Open(file_id_, 7);
  ASSERT_FALSE(scheduler->scheduled().empty());
  const auto request_id = scheduler->scheduled().front().request_id;
  runtime->service->NotifyImageAcquired(runtime->service->identity().session_generation, true);
  runtime->coordinator->NotifySchedulerCompleted(request_id, true);
  runtime->coordinator->NotifyFrameSubmitted(request_id);
  runtime->coordinator->NotifyFramePresented(request_id);
  ASSERT_EQ(runtime->service->state(), alcedo::EditorSessionState::Interactive);

  const auto                                 first       = MakeExposureTransaction(1.0f, 1);
  const auto                                 replacement = MakeExposureTransaction(2.0f, 2);
  const std::vector<alcedo::EditTransaction> before{first};
  std::string                                error;
  ASSERT_TRUE(runtime->service->RecordFinalizedEdit(first, &error)) << error;
  ASSERT_TRUE(runtime->service->RecordHistoryCursorMove(1, 0, &error)) << error;
  ASSERT_TRUE(runtime->service->RecordTimelineRewrite(
      alcedo::ComputeEditorTimelineHash(before, 0),
      alcedo::ComputeEditorTransactionSpanHash(before, 0, before.size()), 0, replacement, &error))
      << error;
  ASSERT_TRUE(
      port_->CommitJournal(file_id_, runtime->service->identity().session_generation, &error)
          .durable)
      << error;

  alcedo::EditorJournalWriter reopened(
      {file_id_, {}, runtime->service->identity().session_generation, 1}, journal_path_);
  const auto decoded = reopened.journal().DecodeRecordChain();
  ASSERT_GE(decoded.records.size(), 4u);
  EXPECT_EQ(decoded.records[0].record_type, alcedo::EditorJournalRecordType::EditAppend);
  EXPECT_EQ(decoded.records[1].record_type, alcedo::EditorJournalRecordType::CursorMove);
  EXPECT_EQ(decoded.records[2].record_type, alcedo::EditorJournalRecordType::RewriteTimeline);
  EXPECT_EQ(decoded.records[3].record_type, alcedo::EditorJournalRecordType::JournalBatchCommit);
}

TEST_F(EditorSessionProductionJournalPortTest,
       MaterializeWritesHistoryPipelineAndRecoveryMetadata) {
  std::string error;
  ASSERT_TRUE(port_->RecordEdit(file_id_, 1, MakeExposureTransaction(1.5f, 1), &error));
  ASSERT_TRUE(port_->CommitJournal(file_id_, 1, &error).durable);

  const auto outcome = port_->Materialize(file_id_, 1, &error);
  ASSERT_TRUE(outcome.accepted) << outcome.error;
  ASSERT_TRUE(outcome.materialized);
  EXPECT_GT(outcome.materialized_operation_sequence, 0u);

  auto metadata = storage_->GetElementController().GetEditorRecoveryMetadata(file_id_);
  ASSERT_TRUE(metadata.has_value());
  EXPECT_EQ(metadata->materialized_operation_sequence, outcome.materialized_operation_sequence);
  auto history = storage_->GetElementController().GetEditHistoryByFileId(file_id_);
  ASSERT_NE(history, nullptr);
  EXPECT_EQ(history->GetActiveVersion().GetAllEditTransactions().size(), 1u);
}

TEST_F(EditorSessionProductionJournalPortTest,
       MaterializeCompactsTheJournalToAMaterializedCheckpoint) {
  std::string error;
  ASSERT_TRUE(port_->RecordEdit(file_id_, 1, MakeExposureTransaction(1.5f, 1), &error));
  ASSERT_TRUE(port_->CommitJournal(file_id_, 1, &error).durable);
  ASSERT_TRUE(port_->Materialize(file_id_, 1, &error).accepted);

  // The on-disk journal is compacted to a CompactionCheckpoint + batch commit.
  const alcedo::EditorJournalIdentity identity{file_id_, alcedo::Hash128{}, 1, 1};
  alcedo::EditorJournalWriter         recovered(identity, journal_path_);
  const auto                          decoded = recovered.journal().DecodeRecordChain();
  ASSERT_FALSE(decoded.records.empty());
  EXPECT_EQ(decoded.records.front().record_type,
            alcedo::EditorJournalRecordType::CompactionCheckpoint);
}

TEST_F(EditorSessionProductionJournalPortTest, MaterializeWithoutNewOperationsDoesNotCompactAgain) {
  std::string error;
  ASSERT_TRUE(port_->RecordEdit(file_id_, 1, MakeExposureTransaction(1.5f, 1), &error));
  ASSERT_TRUE(port_->CommitJournal(file_id_, 1, &error).durable);
  ASSERT_TRUE(port_->Materialize(file_id_, 1, &error).materialized);

  {
    alcedo::EditorJournalWriter after_first_materialize({file_id_, {}, 1, 1}, journal_path_);
    const auto                  decoded = after_first_materialize.journal().DecodeRecordChain();
    ASSERT_FALSE(decoded.records.empty());
    EXPECT_EQ(decoded.records.front().identity.journal_generation, 2u);
  }

  const auto generation_transition = port_->Materialize(file_id_, 1, &error);
  ASSERT_TRUE(generation_transition.accepted) << generation_transition.error;
  EXPECT_TRUE(generation_transition.materialized);
  EXPECT_EQ(generation_transition.materialized_operation_sequence, 0u);

  const auto no_op = port_->Materialize(file_id_, 1, &error);
  ASSERT_TRUE(no_op.accepted) << no_op.error;
  EXPECT_FALSE(no_op.materialized);

  alcedo::EditorJournalWriter reopened({file_id_, {}, 1, 1}, journal_path_);
  const auto                  decoded = reopened.journal().DecodeRecordChain();
  ASSERT_FALSE(decoded.records.empty());
  EXPECT_EQ(decoded.records.front().identity.journal_generation, 2u);
}

TEST_F(EditorSessionProductionJournalPortTest,
       RecoverAndMaterializeRedoesUnmaterializedEditOnReopen) {
  // Simulate a crash after the journal commit but before materialization: the
  // edit is durable in the journal but absent from DuckDB.
  std::string error;
  ASSERT_TRUE(port_->RecordEdit(file_id_, 1, MakeExposureTransaction(2.0f, 1), &error));
  ASSERT_TRUE(port_->CommitJournal(file_id_, 1, &error).durable);
  // A fresh port simulates a process restart against the same DuckDB + journal.
  auto reopened = std::make_unique<alcedo::ui::EditorSessionProductionJournalPort>(MakeServices());

  const auto outcome = reopened->RecoverAndMaterialize(file_id_, 1, &error);
  ASSERT_TRUE(outcome.accepted) << outcome.error;
  ASSERT_TRUE(outcome.materialized);
  EXPECT_GT(outcome.materialized_operation_sequence, 0u);

  auto metadata = storage_->GetElementController().GetEditorRecoveryMetadata(file_id_);
  ASSERT_TRUE(metadata.has_value());
  EXPECT_EQ(metadata->materialized_operation_sequence, outcome.materialized_operation_sequence);
  auto history = storage_->GetElementController().GetEditHistoryByFileId(file_id_);
  ASSERT_NE(history, nullptr);
  EXPECT_EQ(history->GetActiveVersion().GetAllEditTransactions().size(), 1u);
}

TEST_F(EditorSessionProductionJournalPortTest,
       RecoverAndMaterializeEmitsDiagnosticBundleOnFailure) {
  // Force a replay-time recovery failure by recording a RewriteTimeline whose
  // expected_timeline_hash cannot match the seeded (empty) timeline. The port
  // must emit a diagnostic bundle next to the journal from this real recovery
  // failure path (Phase 5H-Fix #7), not only from a test calling
  // WriteEditorJournalDiagnosticBundle directly.
  const alcedo::Hash128 wrong_expected_hash{0x1234567890ABCDEFull, 0xFEDCBA0987654321ull};
  std::string           error;
  ASSERT_TRUE(port_->RecordRewriteTimeline(file_id_, 1, wrong_expected_hash, alcedo::Hash128{}, 0,
                                           MakeExposureTransaction(2.0f, 1), &error))
      << error;
  ASSERT_TRUE(port_->CommitJournal(file_id_, 1, &error).durable);

  const auto outcome = port_->RecoverAndMaterialize(file_id_, 1, &error);
  EXPECT_FALSE(outcome.accepted);

  bool       found_diagnostic = false;
  const auto stem             = journal_path_.filename().string() + ".diagnostic.";
  for (const auto& entry : std::filesystem::directory_iterator(journal_dir_)) {
    const auto name = entry.path().filename().string();
    if (name.find(stem) == 0) {
      found_diagnostic = true;
      EXPECT_TRUE(std::filesystem::exists(entry.path() / "journal.bin"));
      EXPECT_TRUE(std::filesystem::exists(entry.path() / "reason.txt"));
      std::error_code ec;
      std::filesystem::remove_all(entry.path(), ec);
      break;
    }
  }
  EXPECT_TRUE(found_diagnostic);
}

}  // namespace
}  // namespace alcedo::ui
