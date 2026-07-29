//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_session_history_port.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

#include "app/editor_adjustment_pipeline.hpp"
#include "app/editor_mini_git_materializer.hpp"
#include "app/pipeline_service.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"
#include "ui/alcedo_main/album_backend/editor_history_commit_presentation.hpp"
#include "ui/alcedo_main/album_backend/editor_history_shared_helpers.hpp"

namespace alcedo::ui {
namespace {

auto MakeMiniGitPipelineGuard(sl_element_id_t element_id)
    -> std::shared_ptr<alcedo::PipelineGuard> {
  auto guard       = std::make_shared<alcedo::PipelineGuard>();
  guard->id_       = element_id;
  guard->pipeline_ = std::make_shared<alcedo::CPUPipelineExecutor>();
  guard->commit_graph_ =
      std::make_shared<alcedo::CommitGraph>(alcedo::CommitGraph::CreateEmpty(element_id));
  guard->root_id_                  = guard->commit_graph_->GetRootId();
  guard->transaction_chain_hash_   = alcedo::ComputeRootChainHash(guard->root_id_);
  guard->working_head_commit_hash_ = std::nullopt;
  return guard;
}

auto CommitSettled(EditorSessionHistoryPort& port, const alcedo::EditorHistoryGuardHandle& handle,
                   const std::string& field, const std::string& after_json, std::string* error)
    -> bool {
  alcedo::EditorAdjustmentPatch preview{field, after_json, false};
  if (!port.CaptureAdjustmentBeforePreview(handle, preview, error)) return false;
  alcedo::EditorAdjustmentPatch settled{field, after_json, true};
  return port.CommitAdjustment(handle, settled, error);
}

auto PatchValue(const alcedo::EditorRenderAdjustmentSnapshot& snapshot, const std::string& field)
    -> std::string {
  for (const auto& patch : snapshot.patches) {
    if (patch.field_key == field) return patch.params_json;
  }
  return {};
}

class EditorSessionHistoryPortTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RegisterAllOperators();
    const auto stamp =
        std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    journal_path_ = std::filesystem::temp_directory_path() / ("session_history_" + stamp + ".wal");
    guard_        = MakeMiniGitPipelineGuard(42);
    root_graph_   = std::make_shared<alcedo::CommitGraph>(*guard_->commit_graph_);
    pipeline_     = std::make_shared<EditorSessionPipelinePort>();
    pipeline_->SetServices(
        EditorSessionPipelineServices{{}, [guard = guard_](sl_element_id_t) { return guard; }});
    history_.SetServices(
        EditorSessionHistoryPort::Services{[this](sl_element_id_t) { return journal_path_; }});
    history_.SetPipelinePort(pipeline_);
  }

  void TearDown() override {
    history_.Release({42, true});
    std::error_code ec;
    std::filesystem::remove(journal_path_, ec);
  }

  std::filesystem::path                      journal_path_;
  std::shared_ptr<alcedo::PipelineGuard>     guard_;
  std::shared_ptr<alcedo::CommitGraph>       root_graph_;
  std::shared_ptr<EditorSessionPipelinePort> pipeline_;
  EditorSessionHistoryPort                   history_;
};

TEST(EditorHistoryPureReducerTest, ReplaysHeadWithoutConstructingRenderExecutor) {
  auto graph = alcedo::CommitGraph::CreateEmpty(43);
  auto root_snapshot = MakeEmptyCompleteAdjustmentSnapshot();
  const auto spec = alcedo::ResolveEditorAdjustmentField("exposure");
  ASSERT_TRUE(spec.has_value());

  alcedo::OrdinaryEditPayload payload;
  payload.operator_type = spec->operator_type;
  payload.stage_name = spec->stage_name;
  payload.field_name = "$operator_params";
  payload.before_value = nlohmann::json::object();
  payload.after_value = nlohmann::json{{"exposure", 0.75}};
  payload.before_enabled = true;
  payload.after_enabled = true;

  auto expected = root_snapshot;
  std::string error;
  ASSERT_TRUE(ApplyCommittedPayloadToSnapshot(&expected, payload, true, &error)) << error;

  const auto commit = alcedo::EditCommit::MakeEdit(graph.GetRootId(), std::nullopt, payload);
  ASSERT_TRUE(graph.InsertCommit(commit));
  graph.MoveWorkingHead(graph.GetActiveVersionId(), commit.GetCommitHash());

  alcedo::EditorRenderAdjustmentSnapshot actual;
  ASSERT_TRUE(SnapshotAtHead(root_snapshot, graph, commit.GetCommitHash(), &actual, &error))
      << error;
  EXPECT_EQ(PatchValue(actual, "exposure"), PatchValue(expected, "exposure"));
  EXPECT_EQ(actual.patches.size(), kEditorSnapshotFields.size());
  EXPECT_TRUE(IsCompleteAdjustmentSnapshot(actual, &error)) << error;
}

TEST_F(EditorSessionHistoryPortTest, SettledAdjustmentCreatesOneCommitAndUndoRedoMovesHead) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const alcedo::EditorAdjustmentPatch preview{"exposure", R"({"exposure":0.25})", false};
  const alcedo::EditorAdjustmentPatch settled{"exposure", R"({"exposure":0.75})", true};
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, preview, &error)) << error;
  ASSERT_TRUE(history_.CommitAdjustment(handle, settled, &error)) << error;
  ASSERT_EQ(guard_->commit_graph_->CommitCount(), 1u);
  ASSERT_TRUE(history_.Undo(handle, &error)) << error;
  EXPECT_FALSE(guard_->working_head_commit_hash_.has_value());
  ASSERT_TRUE(history_.Redo(handle, &error)) << error;
  EXPECT_TRUE(guard_->working_head_commit_hash_.has_value());
}

TEST_F(EditorSessionHistoryPortTest,
       UndoRedoAcrossTwoParentTintMergeRestoresFirstParentAndResolvedValues) {
  const auto                  root_id        = guard_->commit_graph_->GetRootId();
  const auto                  active_version = guard_->commit_graph_->GetActiveVersionId();

  alcedo::OrdinaryEditPayload current_payload;
  current_payload.operator_type  = alcedo::OperatorType::TINT;
  current_payload.stage_name     = alcedo::PipelineStageName::Color_Adjustment;
  current_payload.field_name     = "$operator_params";
  current_payload.before_value   = nlohmann::json{{"tint", 0.0}};
  current_payload.after_value    = nlohmann::json{{"tint", 2.0}};
  current_payload.before_enabled = true;
  current_payload.after_enabled  = true;
  const auto current_commit =
      alcedo::EditCommit::MakeEdit(root_id, std::nullopt, std::move(current_payload));
  const auto current_head = current_commit.GetCommitHash();
  ASSERT_TRUE(guard_->commit_graph_->InsertCommit(current_commit));
  guard_->commit_graph_->MoveWorkingHead(active_version, current_head);
  guard_->working_head_commit_hash_ = current_head;
  guard_->transaction_chain_hash_   = guard_->commit_graph_->ChainHashForHead(current_head);
  {
    std::unique_lock<std::mutex> render_lock(guard_->pipeline_->GetRenderLock());
    auto& stage   = guard_->pipeline_->GetStage(alcedo::PipelineStageName::Color_Adjustment);
    auto& globals = guard_->pipeline_->GetGlobalParams();
    stage.SetOperator(alcedo::OperatorType::TINT, nlohmann::json{{"tint", 2.0}}, globals);
    stage.EnableOperator(alcedo::OperatorType::TINT, true, globals);
  }

  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;

  alcedo::OrdinaryEditPayload incoming_payload;
  incoming_payload.operator_type  = alcedo::OperatorType::TINT;
  incoming_payload.stage_name     = alcedo::PipelineStageName::Color_Adjustment;
  incoming_payload.field_name     = "$operator_params";
  incoming_payload.before_value   = nlohmann::json{{"tint", 0.0}};
  incoming_payload.after_value    = nlohmann::json{{"tint", 10.0}};
  incoming_payload.before_enabled = true;
  incoming_payload.after_enabled  = true;
  const auto incoming_commit =
      alcedo::EditCommit::MakeEdit(root_id, std::nullopt, std::move(incoming_payload));
  const auto incoming_head = incoming_commit.GetCommitHash();
  ASSERT_TRUE(guard_->commit_graph_->InsertCommit(incoming_commit));

  alcedo::MergeEditPayload merge_payload;
  merge_payload.fields.push_back(alcedo::MergeFieldDelta{
      alcedo::OperatorType::TINT, alcedo::PipelineStageName::Color_Adjustment, "$operator_params",
      nlohmann::json{{"tint", 10.0}}, true});
  const auto merge_commit =
      alcedo::EditCommit::MakeMerge(root_id, current_head, incoming_head, std::move(merge_payload));
  const auto merge_head = merge_commit.GetCommitHash();
  ASSERT_TRUE(guard_->commit_graph_->InsertCommit(merge_commit));
  guard_->commit_graph_->MoveWorkingHead(active_version, merge_head);
  guard_->working_head_commit_hash_ = merge_head;
  guard_->transaction_chain_hash_   = guard_->commit_graph_->ChainHashForHead(merge_head);
  {
    std::unique_lock<std::mutex> render_lock(guard_->pipeline_->GetRenderLock());
    auto& stage   = guard_->pipeline_->GetStage(alcedo::PipelineStageName::Color_Adjustment);
    auto& globals = guard_->pipeline_->GetGlobalParams();
    stage.SetOperator(alcedo::OperatorType::TINT, nlohmann::json{{"tint", 10.0}}, globals);
    stage.EnableOperator(alcedo::OperatorType::TINT, true, globals);
  }

  ASSERT_TRUE(history_.Undo(handle, &error)) << error;
  EXPECT_EQ(guard_->working_head_commit_hash_, current_head);
  alcedo::EditorRenderAdjustmentSnapshot undo_snapshot;
  ASSERT_TRUE(history_.ReadAdjustmentSnapshot(handle, &undo_snapshot, &error)) << error;
  EXPECT_EQ(PatchValue(undo_snapshot, "tint"), R"({"tint":2.0})");
  alcedo::EditorAdjustmentOperatorState tint_state;
  ASSERT_TRUE(
      alcedo::ReadEditorAdjustmentOperatorState(*guard_->pipeline_, "tint", &tint_state, &error))
      << error;
  EXPECT_DOUBLE_EQ(tint_state.params.at("tint").get<double>(), 10.0);

  ASSERT_TRUE(history_.Redo(handle, &error)) << error;
  EXPECT_EQ(guard_->working_head_commit_hash_, merge_head);
  alcedo::EditorRenderAdjustmentSnapshot redo_snapshot;
  ASSERT_TRUE(history_.ReadAdjustmentSnapshot(handle, &redo_snapshot, &error)) << error;
  EXPECT_EQ(PatchValue(redo_snapshot, "tint"), R"({"tint":10.0})");
  ASSERT_TRUE(
      alcedo::ReadEditorAdjustmentOperatorState(*guard_->pipeline_, "tint", &tint_state, &error))
      << error;
  EXPECT_DOUBLE_EQ(tint_state.params.at("tint").get<double>(), 10.0);
}

TEST_F(EditorSessionHistoryPortTest,
       CaptureAndCommitDoNotBlockGuiWhilePipelineRenderLockHeld) {
  // Multi-slider hang repro: worker holds GetRenderLock() for a long Apply
  // (and may wait on the GUI for present). Capture/Commit on the "GUI" thread
  // must not block on that lock — otherwise finish(A) / start(B) freezes the UI.
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;

  // Seed committed snapshot so Capture prefers it (no lock).
  {
    const alcedo::EditorAdjustmentPatch seed{"saturation", R"({"saturation":0})", true};
    ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, seed, &error)) << error;
    ASSERT_TRUE(history_.CommitAdjustment(handle, seed, &error)) << error;
  }

  std::atomic<bool> worker_ready{false};
  std::atomic<bool> release_worker{false};
  std::thread       worker([&] {
    std::unique_lock<std::mutex> held(guard_->pipeline_->GetRenderLock());
    worker_ready.store(true);
    while (!release_worker.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  });
  const auto ready_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!worker_ready.load() && std::chrono::steady_clock::now() < ready_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_TRUE(worker_ready.load());

  // Rapid multi-field handoff while the lock is held for ~seconds.
  const auto t0 = std::chrono::steady_clock::now();
  const alcedo::EditorAdjustmentPatch sat_preview{"saturation", R"({"saturation":20})", false};
  const alcedo::EditorAdjustmentPatch sat_settled{"saturation", R"({"saturation":40})", true};
  const alcedo::EditorAdjustmentPatch vib_preview{"vibrance", R"({"vibrance":10})", false};
  const alcedo::EditorAdjustmentPatch vib_settled{"vibrance", R"({"vibrance":30})", true};

  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, sat_preview, &error)) << error;
  ASSERT_TRUE(history_.CommitAdjustment(handle, sat_settled, &error)) << error;
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, vib_preview, &error)) << error;
  ASSERT_TRUE(history_.CommitAdjustment(handle, vib_settled, &error)) << error;

  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - t0)
                              .count();
  EXPECT_LT(elapsed_ms, 250)
      << "Capture/Commit blocked " << elapsed_ms
      << "ms while render lock held (GUI multi-slider hang class)";

  release_worker.store(true);
  if (worker.joinable()) {
    worker.join();
  }

  alcedo::EditorRenderAdjustmentSnapshot snap;
  ASSERT_TRUE(history_.ReadAdjustmentSnapshot(handle, &snap, &error)) << error;
  // Both fields should be in the committed snapshot after non-blocking commits.
  bool has_sat = false;
  bool has_vib = false;
  for (const auto& p : snap.patches) {
    if (p.field_key == "saturation") has_sat = true;
    if (p.field_key == "vibrance") has_vib = true;
  }
  EXPECT_TRUE(has_sat);
  EXPECT_TRUE(has_vib);
}

TEST_F(EditorSessionHistoryPortTest,
       InitialAdjustmentSnapshotContainsEverySupportedFieldBeforeAnyRender) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;

  alcedo::EditorRenderAdjustmentSnapshot snapshot;
  ASSERT_TRUE(history_.ReadAdjustmentSnapshot(handle, &snapshot, &error)) << error;

  const std::set<std::string> expected_fields = {
      "exposure",     "contrast",  "white",       "black",      "shadows",
      "highlights",   "curve",     "saturation",  "vibrance",   "tint",
      "hls",           "color_wheel", "lut",        "clarity",    "sharpen",
      "odt",           "film_grain", "halation",   "crop_rotate", "raw_decode",
      "lens_calib",   "color_temp"};
  std::set<std::string> actual_fields;
  for (const auto& patch : snapshot.patches) {
    actual_fields.insert(patch.field_key);
    EXPECT_FALSE(patch.params_json.empty());
  }
  EXPECT_EQ(actual_fields, expected_fields);
}

TEST_F(EditorSessionHistoryPortTest,
       UnsupportedAdjustmentFieldFailsBeforeMiniGitPublication) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto commit_count_before = guard_->commit_graph_->CommitCount();

  const alcedo::EditorAdjustmentPatch unsupported{"not_a_supported_adjustment", R"({})", false};
  EXPECT_FALSE(history_.CaptureAdjustmentBeforePreview(handle, unsupported, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(guard_->commit_graph_->CommitCount(), commit_count_before);
  EXPECT_FALSE(guard_->working_head_commit_hash_.has_value());

  alcedo::MiniGitJournal journal(journal_path_);
  std::string            journal_error;
  ASSERT_TRUE(journal.Load(&journal_error)) << journal_error;
  EXPECT_TRUE(journal.records().empty());
}

TEST_F(EditorSessionHistoryPortTest,
       SaveCaptureReturnsWhilePipelineRenderLockIsHeld) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;

  std::atomic<bool> worker_ready{false};
  std::atomic<bool> release_worker{false};
  std::thread       worker([&] {
    std::unique_lock<std::mutex> held(guard_->pipeline_->GetRenderLock());
    worker_ready.store(true);
    while (!release_worker.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  });
  const auto ready_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!worker_ready.load() && std::chrono::steady_clock::now() < ready_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_TRUE(worker_ready.load());

  auto capture_future = std::async(std::launch::async, [&] {
    std::string capture_error;
    return history_.CaptureSaveCheckpoint(handle, &capture_error);
  });
  EXPECT_EQ(capture_future.wait_for(std::chrono::milliseconds(250)), std::future_status::ready)
      << "save capture waited on the worker render lock";

  release_worker.store(true);
  if (worker.joinable()) {
    worker.join();
  }
  ASSERT_TRUE(capture_future.get());
}

TEST_F(EditorSessionHistoryPortTest,
       ProductionHistoryPortCaptureContainsElementVersionRootHeadHashStateSequenceRangeAndRecords) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const alcedo::EditorAdjustmentPatch first{"exposure", R"({"exposure":0.5})", true};
  const alcedo::EditorAdjustmentPatch second{"exposure", R"({"exposure":1.25})", true};
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, first, &error)) << error;
  ASSERT_TRUE(history_.CommitAdjustment(handle, first, &error)) << error;
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, second, &error)) << error;
  ASSERT_TRUE(history_.CommitAdjustment(handle, second, &error)) << error;

  // Production entry surface — not the project fixture helper.
  auto capture = history_.CaptureSaveCheckpoint(handle, &error);
  ASSERT_TRUE(static_cast<bool>(capture)) << error;
  EXPECT_EQ(capture->element_id, 42u);
  EXPECT_EQ(capture->version_id, guard_->commit_graph_->GetActiveVersionId());
  EXPECT_EQ(capture->root_id, guard_->root_id_);
  EXPECT_EQ(capture->version_id, capture->materialization.image_state.active_version_id);
  EXPECT_EQ(capture->root_id, capture->materialization.image_state.root_id);
  EXPECT_EQ(capture->journal_path, journal_path_);
  ASSERT_EQ(capture->journal_records.size(), 2u);
  ASSERT_TRUE(capture->has_journal_range());
  EXPECT_EQ(*capture->first_journal_sequence, 1u);
  EXPECT_EQ(*capture->last_journal_sequence, 2u);
  EXPECT_EQ(capture->journal_records.front().sequence, 1u);
  EXPECT_EQ(capture->journal_records.back().sequence, 2u);
  EXPECT_EQ(capture->working_head, guard_->working_head_commit_hash_);
  EXPECT_EQ(capture->transaction_chain_hash, guard_->transaction_chain_hash_);
  EXPECT_EQ(capture->materialization.image_state.element_id, 42u);
  ASSERT_TRUE(capture->materialization.image_state.serialized_pipeline_state.has_value());
}

TEST_F(EditorSessionHistoryPortTest,
       DiscardMaterializedJournalThroughDropsLivePrefixForSameSessionCapture) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const alcedo::EditorAdjustmentPatch settled{"exposure", R"({"exposure":0.9})", true};
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, settled, &error)) << error;
  ASSERT_TRUE(history_.CommitAdjustment(handle, settled, &error)) << error;

  auto first = history_.CaptureSaveCheckpoint(handle, &error);
  ASSERT_TRUE(static_cast<bool>(first)) << error;
  ASSERT_TRUE(first->has_journal_range());
  ASSERT_TRUE(history_.DiscardMaterializedJournalThrough(handle, *first->last_journal_sequence,
                                                         &error))
      << error;

  auto second = history_.CaptureSaveCheckpoint(handle, &error);
  ASSERT_TRUE(static_cast<bool>(second)) << error;
  EXPECT_TRUE(second->journal_records.empty());
  EXPECT_FALSE(second->has_journal_range());
}

TEST_F(EditorSessionHistoryPortTest,
       DiscardReturnsToMaterializedHeadAndClearsUnmaterializedHistory) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;

  const alcedo::EditorAdjustmentPatch first{"exposure", R"({"exposure":0.4})", true};
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, first, &error)) << error;
  ASSERT_TRUE(history_.CommitAdjustment(handle, first, &error)) << error;
  ASSERT_TRUE(history_.SyncMaterializedStateAfterCheckpoint(handle, &error)) << error;

  alcedo::EditorRenderAdjustmentSnapshot materialized_snapshot;
  ASSERT_TRUE(history_.ReadAdjustmentSnapshot(handle, &materialized_snapshot, &error)) << error;
  const auto materialized_head = guard_->commit_graph_->GetImageEditState().materialized_head_commit_hash;

  const alcedo::EditorAdjustmentPatch second{"exposure", R"({"exposure":0.9})", true};
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, second, &error)) << error;
  ASSERT_TRUE(history_.CommitAdjustment(handle, second, &error)) << error;
  EXPECT_TRUE(history_.HasUnmaterializedChanges(handle, &error)) << error;

  ASSERT_TRUE(history_.DiscardUnmaterializedChanges(handle, &error)) << error;
  EXPECT_FALSE(history_.HasUnmaterializedChanges(handle, &error)) << error;
  EXPECT_EQ(guard_->working_head_commit_hash_, materialized_head);
  EXPECT_FALSE(guard_->dirty_);
  EXPECT_TRUE(guard_->commit_graph_->GetImageEditState().materialized_head_commit_hash.has_value());

  alcedo::EditorRenderAdjustmentSnapshot restored_snapshot;
  ASSERT_TRUE(history_.ReadAdjustmentSnapshot(handle, &restored_snapshot, &error)) << error;
  EXPECT_EQ(PatchValue(restored_snapshot, "exposure"), PatchValue(materialized_snapshot, "exposure"));
  EXPECT_TRUE(history_.CaptureSaveCheckpoint(handle, &error)->journal_records.empty());
}

TEST_F(EditorSessionHistoryPortTest,
       SyncMaterializedStateAfterCheckpointMirrorsActiveHeadIntoInMemoryState) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const alcedo::EditorAdjustmentPatch settled{"exposure", R"({"exposure":0.4})", true};
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, settled, &error)) << error;
  ASSERT_TRUE(history_.CommitAdjustment(handle, settled, &error)) << error;

  const auto active_head = guard_->commit_graph_->GetActiveVersionRef().head_commit_hash;
  ASSERT_TRUE(active_head.has_value()) << "a settled commit must advance the working head";
  // The checkpoint has not reconciled yet; the in-memory materialized head stays at root.
  EXPECT_FALSE(guard_->commit_graph_->GetImageEditState()
                   .materialized_head_commit_hash.has_value())
      << "materialized head must stay at root until a checkpoint materializes it";

  ASSERT_TRUE(history_.SyncMaterializedStateAfterCheckpoint(handle, &error)) << error;
  EXPECT_EQ(guard_->commit_graph_->GetImageEditState().materialized_head_commit_hash,
            active_head);
  EXPECT_EQ(guard_->commit_graph_->GetImageEditState().materialized_transaction_chain_hash,
            guard_->commit_graph_->ChainHashForHead(active_head));
}

TEST_F(EditorSessionHistoryPortTest, JournalAppendFailureKeepsWorkingHeadAtRoot) {
  history_.SetServices(
      EditorSessionHistoryPort::Services{[bad = journal_path_.parent_path() / "not-a-directory"](
                                             sl_element_id_t) { return bad / "image-42.wal"; }});
  {
    std::ofstream blocker(journal_path_.parent_path() / "not-a-directory", std::ios::binary);
    ASSERT_TRUE(blocker.is_open());
    blocker << "block";
  }
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const alcedo::EditorAdjustmentPatch settled{"exposure", R"({"exposure":1.25})", true};
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, settled, &error)) << error;
  EXPECT_FALSE(history_.CommitAdjustment(handle, settled, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(guard_->working_head_commit_hash_.has_value());
  std::error_code ec;
  std::filesystem::remove(journal_path_.parent_path() / "not-a-directory", ec);
}

TEST_F(EditorSessionHistoryPortTest, ReopenReplaysJournalIntoWorkingPipeline) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const alcedo::EditorAdjustmentPatch settled{"exposure", R"({"exposure":1.25})", true};
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, settled, &error)) << error;
  ASSERT_TRUE(history_.CommitAdjustment(handle, settled, &error)) << error;
  history_.Release(handle);

  auto reopened_guard           = MakeMiniGitPipelineGuard(42);
  reopened_guard->commit_graph_ = std::make_shared<alcedo::CommitGraph>(*root_graph_);
  auto reopened_pipeline        = std::make_shared<EditorSessionPipelinePort>();
  reopened_pipeline->SetServices(EditorSessionPipelineServices{
      {}, [reopened_guard](sl_element_id_t) { return reopened_guard; }});
  EditorSessionHistoryPort reopened;
  reopened.SetServices(
      EditorSessionHistoryPort::Services{[this](sl_element_id_t) { return journal_path_; }});
  reopened.SetPipelinePort(reopened_pipeline);
  const auto reopened_handle = reopened.Acquire(42, &error);
  ASSERT_TRUE(reopened_handle.valid) << error;
  EXPECT_EQ(reopened_guard->commit_graph_->CommitCount(), 1u);
  EXPECT_TRUE(reopened_guard->working_head_commit_hash_.has_value());
}

/// Phase 4A: capture returns an owned value; a second capture does not require a
/// side-map TakeSaveCapture, and the same shared_ptr identity reaches a store.
TEST_F(EditorSessionHistoryPortTest, ProductionCaptureValueReachesCheckpointStoreWithoutSideMap) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const alcedo::EditorAdjustmentPatch settled{"exposure", R"({"exposure":0.85})", true};
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, settled, &error)) << error;
  ASSERT_TRUE(history_.CommitAdjustment(handle, settled, &error)) << error;

  auto first = history_.CaptureSaveCheckpoint(handle, &error);
  ASSERT_TRUE(static_cast<bool>(first)) << error;
  ASSERT_TRUE(first->has_journal_range());
  const auto* first_raw = first.get();
  const auto  first_head = first->working_head;
  const auto  first_last = *first->last_journal_sequence;

  // Ownership travels as a function argument. The history port keeps no
  // element-ID side map: a second capture returns an independent value.
  auto second = history_.CaptureSaveCheckpoint(handle, &error);
  ASSERT_TRUE(static_cast<bool>(second)) << error;
  EXPECT_NE(second.get(), first_raw);
  EXPECT_EQ(second->working_head, first_head);
  EXPECT_EQ(*second->last_journal_sequence, first_last);
  // First capture remains valid after the second (no Take/rendezvous).
  EXPECT_EQ(first.get(), first_raw);
  EXPECT_EQ(first->journal_records.size(), second->journal_records.size());
}

TEST_F(EditorSessionHistoryPortTest, HistoryProjectionPublishesDisplayNameBeforeValueAndAfterValue) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  ASSERT_TRUE(CommitSettled(history_, handle, "exposure", R"({"exposure":0.35})", &error)) << error;
  ASSERT_TRUE(CommitSettled(history_, handle, "exposure", R"({"exposure":0.46})", &error)) << error;

  alcedo::EditorHistorySnapshot snapshot;
  ASSERT_TRUE(history_.ReadHistorySnapshot(handle, &snapshot, &error)) << error;
  ASSERT_EQ(snapshot.commits.size(), 2u);
  // Newest-first: the head commit is the second exposure edit.
  const auto& head = snapshot.commits.front();
  EXPECT_EQ(head.field_key, "exposure");
  EXPECT_EQ(head.position, alcedo::EditorHistoryTimelinePosition::Current);
  EXPECT_NE(head.before_value_json.find("0.35"), std::string::npos);
  EXPECT_NE(head.after_value_json.find("0.46"), std::string::npos);

  const auto pres = PresentEditorHistoryCommit(head.field_key, head.before_value_json,
                                               head.after_value_json, head.before_enabled,
                                               head.after_enabled, head.kind, head.merge_field_keys);
  EXPECT_EQ(pres.display_name.toStdString(), "Exposure");
  EXPECT_EQ(pres.before_text.toStdString(), "+0.35");
  EXPECT_EQ(pres.after_text.toStdString(), "+0.46");
  EXPECT_FALSE(pres.icon_key.isEmpty());
  EXPECT_FALSE(pres.is_merge);
}

TEST_F(EditorSessionHistoryPortTest,
       HistoryProjectionMarksOnlyWorkingHeadCurrentAndIncludesRedoSuffix) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  ASSERT_TRUE(CommitSettled(history_, handle, "exposure", R"({"exposure":0.35})", &error)) << error;
  ASSERT_TRUE(CommitSettled(history_, handle, "contrast", R"({"contrast":12.0})", &error)) << error;
  ASSERT_TRUE(CommitSettled(history_, handle, "saturation", R"({"saturation":8.0})", &error)) << error;
  // Move back one hop: head = contrast, saturation enters the redo suffix.
  ASSERT_TRUE(history_.Undo(handle, &error)) << error;

  alcedo::EditorHistorySnapshot snapshot;
  ASSERT_TRUE(history_.ReadHistorySnapshot(handle, &snapshot, &error)) << error;
  EXPECT_TRUE(snapshot.can_undo);
  EXPECT_TRUE(snapshot.can_redo);
  std::size_t current_count = 0;
  std::size_t future_count  = 0;
  std::size_t applied_count = 0;
  alcedo::commit_hash_t current_hash;
  for (const auto& row : snapshot.commits) {
    switch (row.position) {
      case alcedo::EditorHistoryTimelinePosition::Current:
        ++current_count;
        current_hash = row.commit_hash;
        break;
      case alcedo::EditorHistoryTimelinePosition::Future:
        ++future_count;
        break;
      case alcedo::EditorHistoryTimelinePosition::Applied:
        ++applied_count;
        break;
    }
  }
  EXPECT_EQ(current_count, 1u);
  EXPECT_EQ(future_count, 1u);
  EXPECT_EQ(applied_count, 1u);
  // The single Current row is the actual working head (contrast), not the root.
  ASSERT_TRUE(guard_->working_head_commit_hash_.has_value());
  EXPECT_EQ(current_hash, *guard_->working_head_commit_hash_);
}

TEST_F(EditorSessionHistoryPortTest,
       MoveHeadToAncestorThenRedoDescendantPublishesOneFinalSnapshot) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  ASSERT_TRUE(CommitSettled(history_, handle, "exposure", R"({"exposure":0.35})", &error)) << error;
  ASSERT_TRUE(CommitSettled(history_, handle, "contrast", R"({"contrast":12.0})", &error)) << error;
  ASSERT_TRUE(CommitSettled(history_, handle, "saturation", R"({"saturation":8.0})", &error)) << error;
  // Walk the first-parent chain (root -> head) to find the ancestor and head hashes.
  const auto chain = guard_->commit_graph_->FirstParentChain(guard_->working_head_commit_hash_);
  ASSERT_FALSE(chain.empty());
  const auto exposure_id   = guard_->commit_graph_->GetCommit(chain.front()).GetCommitHash();
  const auto saturation_id = guard_->commit_graph_->GetCommit(chain.back()).GetCommitHash();

  auto count_positions = [](const alcedo::EditorHistorySnapshot& s) {
    struct Counts {
      std::size_t current = 0, future = 0, applied = 0;
    } counts;
    for (const auto& row : s.commits) {
      switch (row.position) {
        case alcedo::EditorHistoryTimelinePosition::Current: ++counts.current; break;
        case alcedo::EditorHistoryTimelinePosition::Future: ++counts.future; break;
        case alcedo::EditorHistoryTimelinePosition::Applied: ++counts.applied; break;
      }
    }
    return counts;
  };

  // Backward multi-step: head -> exposure in one operation. The redo suffix
  // keeps contrast + saturation as Future rows.
  ASSERT_TRUE(history_.MoveHeadToCommit(handle, exposure_id, &error)) << error;
  EXPECT_EQ(guard_->working_head_commit_hash_.value(), exposure_id);
  alcedo::EditorHistorySnapshot backward_projection;
  ASSERT_TRUE(history_.ReadHistorySnapshot(handle, &backward_projection, &error)) << error;
  const auto backward_counts = count_positions(backward_projection);
  EXPECT_EQ(backward_counts.current, 1u);
  EXPECT_EQ(backward_counts.future, 2u);
  EXPECT_EQ(backward_counts.applied, 0u);
  alcedo::EditorRenderAdjustmentSnapshot backward_snapshot;
  ASSERT_TRUE(history_.ReadAdjustmentSnapshot(handle, &backward_snapshot, &error)) << error;
  EXPECT_EQ(PatchValue(backward_snapshot, "exposure"), R"({"exposure":0.35})");

  // Forward multi-step: head -> saturation in one operation, consuming the suffix.
  ASSERT_TRUE(history_.MoveHeadToCommit(handle, saturation_id, &error)) << error;
  EXPECT_EQ(guard_->working_head_commit_hash_.value(), saturation_id);
  alcedo::EditorHistorySnapshot forward_projection;
  ASSERT_TRUE(history_.ReadHistorySnapshot(handle, &forward_projection, &error)) << error;
  const auto forward_counts = count_positions(forward_projection);
  EXPECT_EQ(forward_counts.current, 1u);
  EXPECT_EQ(forward_counts.future, 0u);
  EXPECT_EQ(forward_counts.applied, 2u);
  alcedo::EditorRenderAdjustmentSnapshot forward_snapshot;
  ASSERT_TRUE(history_.ReadAdjustmentSnapshot(handle, &forward_snapshot, &error)) << error;
  EXPECT_EQ(PatchValue(forward_snapshot, "saturation"), R"({"saturation":8.0})");
  EXPECT_EQ(PatchValue(forward_snapshot, "contrast"), R"({"contrast":12.0})");
}

TEST(EditorHistoryCommitPresentationTest, FormatsNumericBooleanPathEnumAndCompoundAdjustments) {
  // Numeric slider.
  auto exposure = PresentEditorHistoryCommit("exposure", R"({"exposure":0.0})", R"({"exposure":0.35})",
                                              true, true, alcedo::EditCommitKind::kEdit);
  EXPECT_EQ(exposure.display_name.toStdString(), "Exposure");
  EXPECT_EQ(exposure.before_text.toStdString(), "0");
  EXPECT_EQ(exposure.after_text.toStdString(), "+0.35");

  // Boolean toggle (RAW highlights reconstruct).
  auto raw =
      PresentEditorHistoryCommit("raw_decode", R"({"raw":{"highlights_reconstruct":false}})",
                                 R"({"raw":{"highlights_reconstruct":true}})", true, true,
                                 alcedo::EditCommitKind::kEdit);
  // Path (LUT file).
  auto lut = PresentEditorHistoryCommit("lut", R"({"ocio_lmt":"C:/looks/old.cube"})",
                                        R"({"ocio_lmt":"C:/looks/teal.cube"})", true, true,
                                        alcedo::EditCommitKind::kEdit);
  EXPECT_EQ(lut.display_name.toStdString(), "LUT");
  EXPECT_EQ(lut.before_text.toStdString(), "old.cube");
  EXPECT_EQ(lut.after_text.toStdString(), "teal.cube");

  // Enum (ODT output target).
  auto odt = PresentEditorHistoryCommit(
      "odt", R"({"odt":{"encoding_space":"REC709","encoding_eotf":"GAMMA_2_2","peak_luminance":1000}})",
      R"({"odt":{"encoding_space":"DISPLAY_P3","encoding_eotf":"GAMMA_2_4","peak_luminance":1000}})",
      true, true, alcedo::EditCommitKind::kEdit);
  EXPECT_EQ(odt.display_name.toStdString(), "ODT");
  EXPECT_EQ(odt.before_text.toStdString(), "Rec.709 / Gamma 2.2");
  EXPECT_EQ(odt.after_text.toStdString(), "Display P3 / Gamma 2.4");

  // Compound (crop/rotate angle).
  auto crop = PresentEditorHistoryCommit("crop_rotate", R"({"crop_rotate":{"angle_degrees":0.0}})",
                                         R"({"crop_rotate":{"angle_degrees":12.0}})", true, true,
                                         alcedo::EditCommitKind::kEdit);
  EXPECT_EQ(crop.display_name.toStdString(), "Crop / Rotate");
  EXPECT_EQ(crop.after_text.toStdString(), "+12\u00b0");

  // Merge commit provenance.
  auto merge = PresentEditorHistoryCommit("merge", "", "", true, true,
                                          alcedo::EditCommitKind::kMerge, {"exposure", "contrast"});
  EXPECT_TRUE(merge.is_merge);
  EXPECT_EQ(merge.display_name.toStdString(), "Merge");
  EXPECT_EQ(merge.merge_summary.toStdString(), "Resolved 2 fields");
}

// ---------------------------------------------------------------------------
// Phase 7A R0: failing evidence for Mini-Git atomicity. These tests assert the
// repaired target behavior and fail against the current defect where head moves
// and merge traversal mutate durable/live state before the full target pipeline
// is known to succeed.
// ---------------------------------------------------------------------------

TEST_F(EditorSessionHistoryPortTest,
       HeadMoveApplyFailurePreservesHeadRedoPipelineSnapshotAndJournal) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;

  // Seed one settled exposure edit so the working head and committed snapshot
  // are well-defined before the failing move.
  ASSERT_TRUE(CommitSettled(history_, handle, "exposure", R"({"exposure":0.5})", &error))
      << error;
  const auto c1 = *guard_->working_head_commit_hash_;
  ASSERT_TRUE(guard_->commit_graph_->FindCommit(c1) != nullptr);

  // Insert an in-graph commit whose payload does not map to any QML editor field.
  // MoveHeadToCommit across it must therefore fail ApplyCommittedPayload. The
  // current defect appends the head-move journal record and advances the graph
  // head before the apply is known to succeed, so the failed move leaves the
  // head/redo/journal mutated.
  // Insert an in-graph ordinary commit whose (stage, operator) pair does not
  // map to any QML editor field (EXPOSURE belongs to Basic_Adjustment, not
  // To_WorkingSpace). MoveHeadToCommit across it therefore fails
  // ApplyCommittedPayload at field-key resolution. The current defect appends
  // the head-move journal record and advances the graph head before the apply
  // is known to succeed, so the failed move leaves head/redo/journal mutated.
  alcedo::OrdinaryEditPayload bad_payload;
  bad_payload.operator_type  = alcedo::OperatorType::EXPOSURE;
  bad_payload.stage_name     = alcedo::PipelineStageName::To_WorkingSpace;
  bad_payload.field_name      = "bogus_field";
  bad_payload.after_value     = nlohmann::json::parse(R"({"exposure":0.0})");
  bad_payload.after_enabled   = true;
  auto bad_commit = alcedo::EditCommit::MakeEdit(guard_->commit_graph_->GetRootId(), c1,
                                                 std::move(bad_payload));
  bad_commit.FinalizeHash();
  ASSERT_TRUE(guard_->commit_graph_->InsertCommit(std::move(bad_commit)));
  const auto active_version = guard_->commit_graph_->GetActiveVersionId();
  alcedo::head_commit_hash_t bad_head = std::nullopt;
  for (const auto& [hash, commit] : guard_->commit_graph_->GetAllCommits()) {
    if (commit.GetKind() == alcedo::EditCommitKind::kEdit &&
        commit.GetFirstParentHash() == c1 &&
        commit.GetPayloadJSON().contains("field_name") &&
        commit.GetPayloadJSON()["field_name"] == "bogus_field") {
      bad_head = hash;
      break;
    }
  }
  ASSERT_TRUE(bad_head.has_value()) << "could not locate the inserted bogus commit";
  guard_->commit_graph_->MoveWorkingHead(active_version, bad_head);

  const auto journal_records_before = [&] {
    alcedo::MiniGitJournal j(journal_path_);
    std::string            load_error;
    if (!j.Load(&load_error)) return std::size_t{0};
    return j.records().size();
  }();

  // The move across the bogus commit must fail and leave the full working-state
  // tuple unchanged (R4 prepared-transition target).
  EXPECT_FALSE(history_.MoveHeadToCommit(handle, c1, &error))
      << "a head move across an unmappable commit must fail";

  EXPECT_EQ(guard_->commit_graph_->GetActiveVersionRef().head_commit_hash, bad_head)
      << "a failed head move must not advance the graph head";

  alcedo::EditorHistorySnapshot snapshot;
  ASSERT_TRUE(history_.ReadHistorySnapshot(handle, &snapshot, &error)) << error;
  const auto future_rows = std::count_if(
      snapshot.commits.begin(), snapshot.commits.end(),
      [](const alcedo::EditorHistoryCommit& row) {
        return row.position == alcedo::EditorHistoryTimelinePosition::Future;
      });
  EXPECT_EQ(future_rows, 0) << "a failed head move must not push a redo suffix";

  const auto journal_records_after = [&] {
    alcedo::MiniGitJournal j(journal_path_);
    std::string            load_error;
    if (!j.Load(&load_error)) return std::size_t{0};
    return j.records().size();
  }();
  EXPECT_EQ(journal_records_after, journal_records_before)
      << "a failed head move must not append a journal record";
}

TEST_F(EditorSessionHistoryPortTest, MoveAcrossMergeReconstructsResolvedFields) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;

  // Seed one settled exposure edit (C1) on the active first-parent chain.
  ASSERT_TRUE(CommitSettled(history_, handle, "exposure", R"({"exposure":0.5})", &error))
      << error;
  const auto c1 = *guard_->working_head_commit_hash_;
  const auto active_version = guard_->commit_graph_->GetActiveVersionId();
  const auto root_id = guard_->commit_graph_->GetRootId();

  // Create a second parent (C2) on a parallel root branch for the merge.
  alcedo::OrdinaryEditPayload contrast_payload;
  contrast_payload.operator_type = alcedo::OperatorType::CONTRAST;
  contrast_payload.stage_name     = alcedo::PipelineStageName::Basic_Adjustment;
  contrast_payload.field_name      = "contrast";
  contrast_payload.after_value      = nlohmann::json::parse(R"({"contrast":0.3})");
  contrast_payload.after_enabled    = true;
  auto c2_commit = alcedo::EditCommit::MakeEdit(root_id, std::nullopt, std::move(contrast_payload));
  c2_commit.FinalizeHash();
  ASSERT_TRUE(guard_->commit_graph_->InsertCommit(std::move(c2_commit)));
  alcedo::commit_hash_t c2{};
  for (const auto& [hash, commit] : guard_->commit_graph_->GetAllCommits()) {
    if (commit.GetKind() == alcedo::EditCommitKind::kEdit && !commit.GetFirstParentHash().has_value() &&
        commit.GetPayloadJSON().contains("field_name") &&
        commit.GetPayloadJSON()["field_name"] == "contrast") {
      c2 = hash;
      break;
    }
  }
  ASSERT_NE(c2, alcedo::commit_hash_t{}) << "could not locate the second parent commit";

  // Create a merge commit M (first parent C1, second parent C2) carrying a
  // resolved exposure value distinct from C1.
  alcedo::MergeEditPayload merge_payload;
  alcedo::MergeFieldDelta  field;
  field.operator_type    = alcedo::OperatorType::EXPOSURE;
  field.stage_name        = alcedo::PipelineStageName::Basic_Adjustment;
  field.field_name        = "exposure";
  field.resolved_value     = nlohmann::json::parse(R"({"exposure":0.9})");
  field.resolved_enabled   = true;
  merge_payload.fields.push_back(field);
  merge_payload.CanonicalizeAndValidate();
  auto merge = alcedo::EditCommit::MakeMerge(root_id, c1, c2, std::move(merge_payload));
  merge.FinalizeHash();
  ASSERT_TRUE(guard_->commit_graph_->InsertCommit(std::move(merge)));
  alcedo::commit_hash_t merge_hash{};
  for (const auto& [hash, commit] : guard_->commit_graph_->GetAllCommits()) {
    if (commit.GetKind() == alcedo::EditCommitKind::kMerge &&
        commit.GetFirstParentHash() == c1) {
      merge_hash = hash;
      break;
    }
  }
  ASSERT_NE(merge_hash, alcedo::commit_hash_t{}) << "could not locate the merge commit";
  guard_->commit_graph_->MoveWorkingHead(active_version, merge_hash);

  // Move back to C1 (the merge lands in the redo suffix), then forward to the
  // merge. The forward move must reconstruct the merge's resolved exposure.
  ASSERT_TRUE(history_.MoveHeadToCommit(handle, c1, &error)) << error;
  alcedo::EditorRenderAdjustmentSnapshot baseline_snapshot;
  ASSERT_TRUE(history_.ReadAdjustmentSnapshot(handle, &baseline_snapshot, &error)) << error;
  EXPECT_EQ(PatchValue(baseline_snapshot, "exposure"), R"({"exposure":0.5})");

  ASSERT_TRUE(history_.MoveHeadToCommit(handle, merge_hash, &error)) << error;
  alcedo::EditorRenderAdjustmentSnapshot resolved_snapshot;
  ASSERT_TRUE(history_.ReadAdjustmentSnapshot(handle, &resolved_snapshot, &error)) << error;
  EXPECT_EQ(PatchValue(resolved_snapshot, "exposure"), R"({"exposure":0.9})")
      << "moving forward across a merge must reconstruct the merge-resolved field values";
}
}  // namespace
}  // namespace alcedo::ui
