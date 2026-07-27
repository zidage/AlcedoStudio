//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_session_history_port.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>

#include "app/editor_adjustment_pipeline.hpp"
#include "app/editor_mini_git_materializer.hpp"
#include "app/pipeline_service.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"
#include "ui/alcedo_main/album_backend/editor_history_commit_presentation.hpp"

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

}  // namespace
}  // namespace alcedo::ui
