//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <thread>
#include <variant>
#include <vector>

#include "app/editor_pipeline_command_service.hpp"
#include "app/editor_history_types.hpp"
#include "app/pipeline_document_history.hpp"
#include "app/pipeline_service.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/i_node_model.hpp"
#include "edit/graph/pipeline_graph_commands.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "edit/history/pipeline_edit_batch.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "grade_owned_mask_support.hpp"
#include "support/editor_parameter_target_test.hpp"
#include "ui/alcedo_main/album_backend/editor_history_commit_presentation.hpp"
#include "ui/alcedo_main/album_backend/editor_session_history_port.hpp"

namespace alcedo::ui {
namespace {

using test::ColorGradeFieldTarget;
using test::DrtPostFieldTarget;
using test::WithColorGradeTarget;
using test::WithDrtPostTarget;

/// Real document, executor, history and WAL; no project/storage/UI fixture is needed.
class EditorDocumentHistoryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    journal_path_    = std::filesystem::path(TEST_IMG_PATH)
                        .parent_path()
                        .parent_path()
                        .parent_path()
                        .parent_path() /
                    "build/tmp/nm1" / ("document_history_" + stamp + ".wal");
    std::filesystem::create_directories(journal_path_.parent_path());
    guard_                = std::make_shared<PipelineGuard>();
    guard_->id_           = 42;
    guard_->pipeline_     = std::make_shared<CPUPipelineExecutor>();
    guard_->document_     = std::make_shared<PipelineDocument>(CreateDefaultPipelineDocument());
    guard_->commit_graph_ = std::make_shared<CommitGraph>(CommitGraph::CreateEmpty(42));
    guard_->root_id_      = guard_->commit_graph_->GetRootId();
    guard_->root_document_ =
        std::make_shared<PipelineDocument>(ClonePipelineDocument(*guard_->document_));
    pipeline_             = std::make_shared<EditorSessionPipelinePort>();
    pipeline_->SetServices(
        EditorSessionPipelineMappers{{}, [this](sl_element_id_t) { return guard_; }});
    history_.SetPipelinePort(pipeline_);
    history_.SetServices(
        EditorSessionHistoryPort::Services{[this](sl_element_id_t) { return journal_path_; }});
  }
  void TearDown() override {
    history_.Release({42, true});
    std::error_code ec;
    std::filesystem::remove(journal_path_, ec);
  }
  std::filesystem::path                      journal_path_;
  std::shared_ptr<PipelineGuard>             guard_;
  std::shared_ptr<EditorSessionPipelinePort> pipeline_;
  EditorSessionHistoryPort                   history_;
};

auto DocumentExposureEv(const alcedo::PipelineDocument& document, const std::string& node_id)
    -> float {
  nlohmann::json json;
  std::string    error;
  EXPECT_TRUE(alcedo::ReadEditorParameterJson(document, ColorGradeFieldTarget("exposure", node_id),
                                              &json, &error))
      << error;
  return json.at("exposure_ev").get<float>();
}

auto DocumentClarity(const alcedo::PipelineDocument& document) -> float {
  nlohmann::json json;
  std::string    error;
  EXPECT_TRUE(alcedo::ReadEditorParameterJson(document, DrtPostFieldTarget("clarity"), &json, &error))
      << error;
  return json.at("clarity").get<float>();
}

TEST_F(EditorDocumentHistoryTest, ProtectedGradeBatchRemovalKeepsDocumentAndHistoryUnchanged) {
  std::string error;
  const auto handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto before = guard_->document_->ToJson();
  const auto count = guard_->commit_graph_->CommitCount();
  const auto head = guard_->working_head_commit_hash();
  auto batch = alcedo::MakeRemoveColorGradeBatch(
      alcedo::CaptureRemoveColorGradeChange(*guard_->document_, NodeId{"grade.primary"}));
  EXPECT_FALSE(history_.CommitPipelineEditBatch(handle, std::move(batch), &error));
  EXPECT_NE(error.find("grade.primary"), std::string::npos);
  EXPECT_EQ(guard_->document_->ToJson(), before);
  EXPECT_EQ(guard_->working_head_commit_hash(), head);
  EXPECT_EQ(guard_->commit_graph_->CommitCount(), count);
}

TEST_F(EditorDocumentHistoryTest, SettledExposurePatchWritesPrimaryGradeDocumentNotOnlyStages) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  ASSERT_NE(guard_->document_, nullptr);
  const auto before_hash = alcedo::CanonicalPipelineDocumentJson(*guard_->document_);
  auto       settled     = WithColorGradeTarget({"exposure", R"({"exposure_ev":2.25})", true});
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, settled, &error)) << error;
  ASSERT_TRUE(history_.CommitAdjustment(handle, settled, &error)) << error;
  EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_, "grade.primary"), 2.25f);
  EXPECT_NE(alcedo::CanonicalPipelineDocumentJson(*guard_->document_), before_hash);
}

TEST_F(EditorDocumentHistoryTest,
       IncompleteTargetRejectedLeavesDocumentHashAndHistoryHeadUnchanged) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto before_hash = alcedo::CanonicalPipelineDocumentJson(*guard_->document_);
  auto       missing_node = WithColorGradeTarget({"exposure", R"({"exposure_ev":3.0})", false});
  missing_node.target.node_id = alcedo::NodeId{};
  EXPECT_FALSE(history_.CaptureAdjustmentBeforePreview(handle, missing_node, &error));
  EXPECT_EQ(error, "Editor parameter target requires node_id");
  EXPECT_EQ(alcedo::CanonicalPipelineDocumentJson(*guard_->document_), before_hash);
  EXPECT_FALSE(guard_->working_head_commit_hash().has_value());
  EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_, "grade.primary"),
                  alcedo::kDefaultPipelineExposureEv);
}

TEST_F(EditorDocumentHistoryTest, ProvisionalSequenceReusesTargetResolvedAtFirstPatch) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  auto add_errors = alcedo::AddCleanColorGrade(*guard_->document_, alcedo::NodeId{"drt"},
                                               alcedo::NodeId{"grade.extra"});
  ASSERT_TRUE(add_errors.empty());
  EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_, "grade.extra"), 0.0f);

  auto first = WithColorGradeTarget({"exposure", R"({"exposure_ev":2.0})", false}, "grade.primary");
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, first, &error)) << error;
  auto second = WithColorGradeTarget({"exposure", R"({"exposure_ev":4.0})", false}, "grade.extra");
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, second, &error)) << error;
  auto settled = WithColorGradeTarget({"exposure", R"({"exposure_ev":4.0})", true}, "grade.extra");
  ASSERT_TRUE(history_.CommitAdjustment(handle, settled, &error)) << error;

  EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_, "grade.primary"), 4.0f);
  EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_, "grade.extra"), 0.0f);
}

TEST_F(EditorDocumentHistoryTest, UnknownFieldRejectedLeavesDocumentHashAndHistoryHeadUnchanged) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto before_hash = alcedo::CanonicalPipelineDocumentJson(*guard_->document_);
  auto       patch       = WithColorGradeTarget({"not_a_supported_adjustment", R"({})", false});
  EXPECT_FALSE(history_.CaptureAdjustmentBeforePreview(handle, patch, &error));
  EXPECT_EQ(error, "Unknown editor adjustment field: not_a_supported_adjustment");
  EXPECT_EQ(alcedo::CanonicalPipelineDocumentJson(*guard_->document_), before_hash);
  EXPECT_FALSE(guard_->working_head_commit_hash().has_value());
}

TEST_F(EditorDocumentHistoryTest, UndoSettledExposureRestoresDocumentValue) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  auto settled = WithColorGradeTarget({"exposure", R"({"exposure_ev":2.5})", true});
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, settled, &error)) << error;
  ASSERT_TRUE(history_.CommitAdjustment(handle, settled, &error)) << error;
  EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_, "grade.primary"), 2.5f);
  ASSERT_TRUE(history_.Undo(handle, &error)) << error;
  EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_, "grade.primary"),
                  alcedo::kDefaultPipelineExposureEv);
  ASSERT_TRUE(history_.Redo(handle, &error)) << error;
  EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_, "grade.primary"), 2.5f);
}

TEST_F(EditorDocumentHistoryTest, IncompleteLaterPatchRejectedLeavesLockedDocumentUnchanged) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  auto first = WithColorGradeTarget({"exposure", R"({"exposure_ev":2.0})", false});
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, first, &error)) << error;
  EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_, "grade.primary"), 2.0f);

  auto missing_node = WithColorGradeTarget({"contrast", R"({"contrast":40.0})", false});
  missing_node.target.node_id = alcedo::NodeId{};
  EXPECT_FALSE(history_.CaptureAdjustmentBeforePreview(handle, missing_node, &error));
  EXPECT_EQ(error, "Editor parameter target requires node_id");
  EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_, "grade.primary"), 2.0f);
  EXPECT_FALSE(guard_->working_head_commit_hash().has_value());
}

TEST_F(EditorDocumentHistoryTest, JournalAppendFailureRestoresDocumentExposureEv) {
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
  auto settled = WithColorGradeTarget({"exposure", R"({"exposure_ev":2.25})", true});
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, settled, &error)) << error;
  EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_, "grade.primary"), 2.25f);
  EXPECT_FALSE(history_.CommitAdjustment(handle, settled, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(guard_->working_head_commit_hash().has_value());
  EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_, "grade.primary"),
                  alcedo::kDefaultPipelineExposureEv);
  std::error_code ec;
  std::filesystem::remove(journal_path_.parent_path() / "not-a-directory", ec);
}

TEST_F(EditorDocumentHistoryTest, CaptureAppliesTypedScalarWriteWhenParamsJsonIsEmpty) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  auto patch = WithColorGradeTarget(test::ScalarPatch("exposure", 2.25f, true));
  patch.params_json.clear();
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, patch, &error)) << error;
  EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_, "grade.primary"), 2.25f);
  ASSERT_TRUE(history_.CommitAdjustment(handle, patch, &error)) << error;
  EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_, "grade.primary"), 2.25f);
}

TEST_F(EditorDocumentHistoryTest, DiscardUncommittedPreviewRestoresDocumentExposureEv) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  auto preview = WithColorGradeTarget({"exposure", R"({"exposure_ev":2.25})", false});
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, preview, &error)) << error;
  EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_, "grade.primary"), 2.25f);
  ASSERT_TRUE(history_.DiscardUnmaterializedChanges(handle, &error)) << error;
  EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_, "grade.primary"),
                  alcedo::kDefaultPipelineExposureEv);
}

TEST_F(EditorDocumentHistoryTest, MaskTargetWriteIsRejected) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto before_hash = alcedo::CanonicalPipelineDocumentJson(*guard_->document_);
  alcedo::EditorAdjustmentPatch patch = test::ScalarPatch("exposure", 3.0f);
  patch.target.owner_kind             = alcedo::EditorParameterOwnerKind::ColorGradeMask;
  patch.target.node_id                = alcedo::NodeId{"grade.primary"};
  patch.target.adjustment_instance_id = alcedo::AdjustmentInstanceId{"grade.primary.exposure"};
  patch.target.mask_id                = "mask.1";
  patch.target.field_key              = "exposure";
  EXPECT_FALSE(history_.CaptureAdjustmentBeforePreview(handle, patch, &error));
  EXPECT_EQ(error, "Mask parameter targets are rejected until NM3");
  EXPECT_EQ(alcedo::CanonicalPipelineDocumentJson(*guard_->document_), before_hash);
  EXPECT_FALSE(guard_->working_head_commit_hash().has_value());
}

TEST_F(EditorDocumentHistoryTest, InvalidParameterLeavesLiveValueAndHistoryHeadUnchanged) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto before = guard_->document_->ToJson();
  auto       bad    = WithColorGradeTarget({"exposure", R"({"exposure_ev":"bad"})", false});
  EXPECT_FALSE(history_.CaptureAdjustmentBeforePreview(handle, bad, &error));
  EXPECT_EQ(guard_->document_->ToJson(), before);
  EXPECT_FALSE(guard_->working_head_commit_hash().has_value());
  EXPECT_EQ(guard_->commit_graph_->CommitCount(), 0u);
  EXPECT_FALSE(guard_->dirty_);

  auto valid = WithColorGradeTarget({"exposure", R"({"exposure_ev":2.0})", true});
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, valid, &error)) << error;
  ASSERT_TRUE(history_.CommitAdjustment(handle, valid, &error)) << error;
  const auto head    = guard_->working_head_commit_hash();
  const auto count   = guard_->commit_graph_->CommitCount();
  auto       preview = WithColorGradeTarget({"exposure", R"({"exposure_ev":3.0})", false});
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, preview, &error)) << error;
  bad.settled = true;
  EXPECT_FALSE(history_.CommitAdjustment(handle, bad, &error));
  EXPECT_EQ(guard_->working_head_commit_hash(), head);
  EXPECT_EQ(guard_->commit_graph_->CommitCount(), count);
  EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_, "grade.primary"), 3.0f);
  ASSERT_TRUE(history_.CommitAdjustment(handle, preview, &error)) << error;
  ASSERT_TRUE(history_.Undo(handle, &error)) << error;
  EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_, "grade.primary"), 2.0f);
}

TEST_F(EditorDocumentHistoryTest,
       RejectedFirstPatchDoesNotLockTargetAndHistoryUsesNormalizedModelValues) {
  ASSERT_TRUE(AddCleanColorGrade(*guard_->document_, NodeId{"drt"}, NodeId{"grade.extra"}).empty());
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  auto bad = WithColorGradeTarget({"exposure", R"({"exposure_ev":null})", false});
  EXPECT_FALSE(history_.CaptureAdjustmentBeforePreview(handle, bad, &error));
  auto valid = WithColorGradeTarget({"exposure", R"({"exposure_ev":100.0})", true}, "grade.extra");
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, valid, &error)) << error;
  ASSERT_TRUE(history_.CommitAdjustment(handle, valid, &error)) << error;
  EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_, "grade.primary"), 1.5f);
  EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_, "grade.extra"), 16.0f);
  const auto head = *guard_->working_head_commit_hash();
  const auto payload =
      PipelineEditBatch::FromJSON(guard_->commit_graph_->GetCommit(head).GetPayloadJSON());
  const auto* parameter = std::get_if<SetParameterChange>(&payload.changes.front());
  ASSERT_NE(parameter, nullptr);
  EXPECT_EQ(parameter->before_value.at("exposure_ev"), 0.0);
  EXPECT_EQ(parameter->after_value.at("exposure_ev"), 16.0);
  // A different raw request normalizes to the same value, so it cannot produce another commit.
  valid.write = alcedo::EditorScalarWrite{20.0f};
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, valid, &error)) << error;
  ASSERT_TRUE(history_.CommitAdjustment(handle, valid, &error)) << error;
  EXPECT_EQ(guard_->working_head_commit_hash(), head);
  ASSERT_TRUE(history_.Undo(handle, &error)) << error;
  EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_, "grade.extra"), 0.0f);
  ASSERT_TRUE(history_.Redo(handle, &error)) << error;
  EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_, "grade.extra"), 16.0f);
  EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_, "grade.primary"), 1.5f);
}

TEST_F(EditorDocumentHistoryTest, PreviewCommitUndoRedoAndCancelWaitForRenderLock) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto* grade       = guard_->document_->PrimaryGrade();
  const auto* exposure    = grade->FindAdjustmentByType(type_ids::Exposure());
  const auto  patch       = WithColorGradeTarget({"exposure", R"({"exposure_ev":2.5})", true});
  const auto  run_blocked = [&](auto action, float before, float after) {
    std::unique_lock   held(guard_->pipeline_->GetRenderLock());
    std::promise<void> started;
    auto               ready  = started.get_future();
    auto               worker = std::async(std::launch::async, [&] {
      std::string local_error;
      started.set_value();
      const bool ok = action(&local_error);
      return std::pair{ok, local_error};
    });
    ready.wait();
    EXPECT_EQ(worker.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout);
    EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_, "grade.primary"), before);
    held.unlock();
    const auto [ok, local_error] = worker.get();
    EXPECT_TRUE(ok) << local_error;
    EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_, "grade.primary"), after);
    EXPECT_EQ(guard_->document_->PrimaryGrade(), grade);
    EXPECT_EQ(grade->FindAdjustmentByType(type_ids::Exposure()), exposure);
  };
  run_blocked([&](auto* e) { return history_.CaptureAdjustmentBeforePreview(handle, patch, e); },
              1.5f, 2.5f);
  run_blocked([&](auto* e) { return history_.CommitAdjustment(handle, patch, e); }, 2.5f, 2.5f);
  run_blocked([&](auto* e) { return history_.Undo(handle, e); }, 2.5f, 1.5f);
  run_blocked([&](auto* e) { return history_.Redo(handle, e); }, 1.5f, 2.5f);
  const auto preview = WithColorGradeTarget({"exposure", R"({"exposure_ev":3.0})", false});
  run_blocked([&](auto* e) { return history_.CaptureAdjustmentBeforePreview(handle, preview, e); },
              2.5f, 3.0f);
  run_blocked([&](auto* e) { return history_.DiscardUnmaterializedChanges(handle, e); }, 3.0f,
              1.5f);
}

TEST_F(EditorDocumentHistoryTest, TypedUndoAfterHistoryReleaseRestoresDocumentFromStoredBatch) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  auto patch = WithColorGradeTarget({"exposure", R"({"exposure_ev":2.5})", true});
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, patch, &error)) << error;
  ASSERT_TRUE(history_.CommitAdjustment(handle, patch, &error)) << error;
  history_.Release(handle);
  const auto reopened = history_.Acquire(42, &error);
  ASSERT_TRUE(reopened.valid) << error;
  const auto head = guard_->working_head_commit_hash();
  ASSERT_TRUE(history_.Undo(reopened, &error)) << error;
  EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_, "grade.primary"), 1.5f);
  EXPECT_NE(guard_->working_head_commit_hash(), head);
}

auto DocumentDevelopPayload(const alcedo::PipelineDocument& document) -> alcedo::DevelopPayload {
  const auto* develop = document.Develop();
  EXPECT_NE(develop, nullptr);
  return develop == nullptr ? alcedo::DevelopPayload{} : develop->Params().Params();
}

/// Regression: RAW import re-binds the Develop camera profile and as-shot white
/// balance outside history. White balance, lens and RAW decode commits store the
/// whole shared Develop JSON, so a strict whole-JSON match made every earlier
/// commit impossible to undo after a re-bind. Undo and redo must match and write
/// only the keys the field owns, and keep the re-bound import data.
TEST_F(EditorDocumentHistoryTest, DevelopCameraProfileRebindDoesNotBlockWhiteBalanceUndoRedo) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto root = DocumentDevelopPayload(*guard_->document_);

  const auto preview = test::PatchFromJson(
      "color_temp", R"({"wb_mode":"custom","custom_cct":7200.0,"custom_tint":4.0})", false);
  const auto settled = test::PatchFromJson(
      "color_temp", R"({"wb_mode":"custom","custom_cct":7200.0,"custom_tint":4.0})", true);
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, preview, &error)) << error;
  ASSERT_TRUE(history_.CommitAdjustment(handle, settled, &error)) << error;
  const auto lens_preview = test::PatchFromJson("lens_calib", R"({"apply_tca":true})", false);
  const auto lens_settled = test::PatchFromJson("lens_calib", R"({"apply_tca":true})", true);
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, lens_preview, &error)) << error;
  ASSERT_TRUE(history_.CommitAdjustment(handle, lens_settled, &error)) << error;

  // Simulate the RAW color context re-bind that happens outside history.
  auto rebound = DocumentDevelopPayload(*guard_->document_);
  rebound.as_shot_cct                  = root.as_shot_cct + 321.0f;
  rebound.as_shot_tint                 = root.as_shot_tint + 3.0f;
  rebound.camera_profile.cam_mul[0]    = root.camera_profile.cam_mul[0] + 0.5f;
  rebound.camera_profile.color_matrix_1_cct = root.camera_profile.color_matrix_1_cct + 100.0;
  guard_->document_->Develop()->Params().ReplaceParams(rebound);

  ASSERT_TRUE(history_.Undo(handle, &error)) << error;
  ASSERT_TRUE(history_.Undo(handle, &error)) << error;
  auto undone = DocumentDevelopPayload(*guard_->document_);
  EXPECT_EQ(undone.wb_mode, root.wb_mode);
  EXPECT_FLOAT_EQ(undone.custom_cct, root.custom_cct);
  EXPECT_FLOAT_EQ(undone.custom_tint, root.custom_tint);
  EXPECT_EQ(undone.apply_tca, root.apply_tca);
  EXPECT_FLOAT_EQ(undone.as_shot_cct, rebound.as_shot_cct);
  EXPECT_FLOAT_EQ(undone.as_shot_tint, rebound.as_shot_tint);
  EXPECT_FLOAT_EQ(undone.camera_profile.cam_mul[0], rebound.camera_profile.cam_mul[0]);
  EXPECT_DOUBLE_EQ(undone.camera_profile.color_matrix_1_cct,
                   rebound.camera_profile.color_matrix_1_cct);

  ASSERT_TRUE(history_.Redo(handle, &error)) << error;
  ASSERT_TRUE(history_.Redo(handle, &error)) << error;
  auto redone = DocumentDevelopPayload(*guard_->document_);
  EXPECT_EQ(redone.wb_mode, "custom");
  EXPECT_FLOAT_EQ(redone.custom_cct, 7200.0f);
  EXPECT_FLOAT_EQ(redone.custom_tint, 4.0f);
  EXPECT_TRUE(redone.apply_tca);
  EXPECT_FLOAT_EQ(redone.as_shot_cct, rebound.as_shot_cct);
  EXPECT_FLOAT_EQ(redone.camera_profile.cam_mul[0], rebound.camera_profile.cam_mul[0]);
}

/// A drift in a key the field owns is still a real conflict: undo fails and leaves
/// the document and working head unchanged.
TEST_F(EditorDocumentHistoryTest, OwnedWhiteBalanceDriftStillRejectsUndo) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto settled =
      test::PatchFromJson("color_temp", R"({"wb_mode":"custom","custom_cct":7200.0})", true);
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, settled, &error)) << error;
  ASSERT_TRUE(history_.CommitAdjustment(handle, settled, &error)) << error;
  auto drifted       = DocumentDevelopPayload(*guard_->document_);
  drifted.custom_cct = 5100.0f;
  guard_->document_->Develop()->Params().ReplaceParams(drifted);
  const auto head = guard_->working_head_commit_hash();

  EXPECT_FALSE(history_.Undo(handle, &error));
  EXPECT_NE(error.find("does not match stored values"), std::string::npos) << error;
  EXPECT_EQ(guard_->working_head_commit_hash(), head);
  EXPECT_FLOAT_EQ(DocumentDevelopPayload(*guard_->document_).custom_cct, 5100.0f);
}

TEST_F(EditorDocumentHistoryTest, PostControlTargetsDrtAndRestoresOnUndo) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  ASSERT_NE(guard_->document_, nullptr);
  const auto filled =
      alcedo::CompleteCurrentPanelParameterTarget(*guard_->document_, "clarity", &error);
  ASSERT_TRUE(filled.has_value()) << error;
  EXPECT_EQ(filled->owner_kind, alcedo::EditorParameterOwnerKind::DrtPost);
  EXPECT_EQ(filled->node_id, alcedo::NodeId{"drt"});
  EXPECT_EQ(filled->adjustment_instance_id, alcedo::AdjustmentInstanceId{"drt.clarity"});
  EXPECT_FLOAT_EQ(DocumentClarity(*guard_->document_), 0.0f);

  alcedo::EditorAdjustmentPatch unspecified = test::ScalarPatch("clarity", 25.0f);
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, unspecified, &error)) << error;
  EXPECT_FLOAT_EQ(DocumentClarity(*guard_->document_), 25.0f);

  auto preview = WithDrtPostTarget({"clarity", R"({"clarity":40.0})", false});
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, preview, &error)) << error;
  EXPECT_FLOAT_EQ(DocumentClarity(*guard_->document_), 40.0f);

  auto settled = WithDrtPostTarget({"clarity", R"({"clarity":40.0})", true});
  ASSERT_TRUE(history_.CommitAdjustment(handle, settled, &error)) << error;
  EXPECT_FLOAT_EQ(DocumentClarity(*guard_->document_), 40.0f);
  ASSERT_TRUE(history_.Undo(handle, &error)) << error;
  EXPECT_FLOAT_EQ(DocumentClarity(*guard_->document_), 0.0f);
  ASSERT_TRUE(history_.Redo(handle, &error)) << error;
  EXPECT_FLOAT_EQ(DocumentClarity(*guard_->document_), 40.0f);

  const auto restored = alcedo::PipelineDocument::FromJson(guard_->document_->ToJson());
  EXPECT_FLOAT_EQ(DocumentClarity(restored), 40.0f);
  EXPECT_EQ(restored.PrimaryGrade()->FindAdjustmentByType(alcedo::type_ids::Clarity()), nullptr);
}

auto BackboneNodeIds(const alcedo::PipelineDocument& document) -> std::vector<std::string> {
  std::vector<std::string> ids;
  const auto*              node = static_cast<const alcedo::INodeModel*>(document.Develop());
  while (node != nullptr) {
    ids.emplace_back(node->Id().Value());
    const auto* edge = alcedo::FindSceneImageSuccessor(document.Graph(), node->Id());
    if (edge == nullptr) {
      break;
    }
    node = document.Graph().FindNode(edge->to_node);
  }
  return ids;
}

auto CommitCapturedAddColorGrade(EditorSessionHistoryPort& history,
                                 const alcedo::EditorHistoryGuardHandle& handle,
                                 alcedo::PipelineDocument& document,
                                 const alcedo::NodeId& before_node_id, const alcedo::NodeId& new_id,
                                 std::string* error) -> bool {
  try {
    auto change = alcedo::CaptureAddColorGradeChange(document, before_node_id, new_id);
    return history.CommitPipelineEditBatch(handle, alcedo::MakeAddColorGradeBatch(std::move(change)),
                                           error);
  } catch (const std::exception& ex) {
    if (error != nullptr) {
      *error = ex.what();
    }
    return false;
  }
}

auto CommitCapturedRemoveColorGrade(EditorSessionHistoryPort& history,
                                    const alcedo::EditorHistoryGuardHandle& handle,
                                    alcedo::PipelineDocument& document, const alcedo::NodeId& node_id,
                                    std::string* error) -> bool {
  try {
    auto change = alcedo::CaptureRemoveColorGradeChange(document, node_id);
    return history.CommitPipelineEditBatch(
        handle, alcedo::MakeRemoveColorGradeBatch(std::move(change)), error);
  } catch (const std::exception& ex) {
    if (error != nullptr) {
      *error = ex.what();
    }
    return false;
  }
}

auto CommitCapturedReconnectColorGrade(EditorSessionHistoryPort& history,
                                       const alcedo::EditorHistoryGuardHandle& handle,
                                       alcedo::PipelineDocument& document,
                                       const alcedo::NodeId& node_id,
                                       const alcedo::NodeId& new_predecessor_id,
                                       const alcedo::NodeId& new_successor_id, std::string* error)
    -> bool {
  try {
    auto change = alcedo::CaptureReconnectColorGradeChange(document, node_id, new_predecessor_id,
                                                           new_successor_id);
    return history.CommitPipelineEditBatch(
        handle, alcedo::MakeReconnectColorGradeBatch(std::move(change)), error);
  } catch (const std::exception& ex) {
    if (error != nullptr) {
      *error = ex.what();
    }
    return false;
  }
}

TEST_F(EditorDocumentHistoryTest, AddGradeUndoRedoPreservesStableIdsAndCleanValues) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  ASSERT_TRUE(CommitCapturedAddColorGrade(history_, handle, *guard_->document_,
                                          alcedo::NodeId{"drt"}, alcedo::NodeId{"grade.extra"},
                                          &error))
      << error;
  const auto* extra = dynamic_cast<const alcedo::ColorGradeNodeModel*>(
      guard_->document_->Graph().FindNode(alcedo::NodeId{"grade.extra"}));
  ASSERT_NE(extra, nullptr);
  const auto stored = extra->ToJson();
  EXPECT_TRUE(extra->Enabled());
  EXPECT_FLOAT_EQ(extra->Mix(), 1.0f);
  EXPECT_EQ(extra->DisplayName(), "Color Grade 2");
  EXPECT_EQ(guard_->document_->NextColorGradeNameNumber(), 3u);
  ASSERT_TRUE(history_.Undo(handle, &error)) << error;
  EXPECT_EQ(guard_->document_->Graph().FindNode(alcedo::NodeId{"grade.extra"}), nullptr);
  EXPECT_EQ(guard_->document_->NextColorGradeNameNumber(), 2u);
  ASSERT_TRUE(history_.Redo(handle, &error)) << error;
  const auto* restored = dynamic_cast<const alcedo::ColorGradeNodeModel*>(
      guard_->document_->Graph().FindNode(alcedo::NodeId{"grade.extra"}));
  ASSERT_NE(restored, nullptr);
  EXPECT_EQ(restored->ToJson(), stored);
  EXPECT_EQ(restored->DisplayName(), "Color Grade 2");
  EXPECT_EQ(guard_->document_->NextColorGradeNameNumber(), 3u);
}

TEST_F(EditorDocumentHistoryTest, DeleteGradeUndoRestoresNodeMasksAndExactEdges) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  ASSERT_TRUE(CommitCapturedAddColorGrade(history_, handle, *guard_->document_,
                                          alcedo::NodeId{"drt"}, alcedo::NodeId{"grade.extra"},
                                          &error))
      << error;
  auto* extra = dynamic_cast<alcedo::ColorGradeNodeModel*>(
      guard_->document_->Graph().FindNode(alcedo::NodeId{"grade.extra"}));
  ASSERT_NE(extra, nullptr);
  extra->AddMask(alcedo::grade_mask_test::MakeRadialMask(alcedo::MaskId{"mask.radial"}), 0);
  extra->AddMask(alcedo::grade_mask_test::MakeLinearGradientMask(alcedo::MaskId{"mask.linear"}), 1);
  const auto node_json = extra->ToJson();
  const auto incoming  = *alcedo::FindSceneImagePredecessor(guard_->document_->Graph(),
                                                           alcedo::NodeId{"grade.extra"});
  const auto outgoing  = *alcedo::FindSceneImageSuccessor(guard_->document_->Graph(),
                                                         alcedo::NodeId{"grade.extra"});
  ASSERT_TRUE(CommitCapturedRemoveColorGrade(history_, handle, *guard_->document_,
                                             alcedo::NodeId{"grade.extra"}, &error))
      << error;
  EXPECT_EQ(guard_->document_->Graph().FindNode(alcedo::NodeId{"grade.extra"}), nullptr);
  ASSERT_TRUE(history_.Undo(handle, &error)) << error;
  const auto* restored = dynamic_cast<const alcedo::ColorGradeNodeModel*>(
      guard_->document_->Graph().FindNode(alcedo::NodeId{"grade.extra"}));
  ASSERT_NE(restored, nullptr);
  EXPECT_EQ(restored->ToJson().dump(), node_json.dump());
  EXPECT_EQ(alcedo::FindSceneImagePredecessor(guard_->document_->Graph(),
                                               alcedo::NodeId{"grade.extra"})
                ->from_node,
            incoming.from_node);
  EXPECT_EQ(alcedo::FindSceneImagePredecessor(guard_->document_->Graph(),
                                               alcedo::NodeId{"grade.extra"})
                ->to_node,
            incoming.to_node);
  EXPECT_EQ(alcedo::FindSceneImageSuccessor(guard_->document_->Graph(),
                                           alcedo::NodeId{"grade.extra"})
                ->to_node,
            outgoing.to_node);
}

TEST_F(EditorDocumentHistoryTest, ReconnectUndoRedoRestoresBackboneOrder) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  ASSERT_TRUE(CommitCapturedAddColorGrade(history_, handle, *guard_->document_,
                                          alcedo::NodeId{"drt"}, alcedo::NodeId{"grade.a"}, &error))
      << error;
  ASSERT_TRUE(CommitCapturedAddColorGrade(history_, handle, *guard_->document_,
                                          alcedo::NodeId{"drt"}, alcedo::NodeId{"grade.b"}, &error))
      << error;
  const auto before = BackboneNodeIds(*guard_->document_);
  ASSERT_TRUE(CommitCapturedReconnectColorGrade(history_, handle, *guard_->document_,
                                                alcedo::NodeId{"grade.b"}, alcedo::NodeId{"develop"},
                                                alcedo::NodeId{"grade.primary"}, &error))
      << error;
  const auto moved = BackboneNodeIds(*guard_->document_);
  EXPECT_NE(moved, before);
  ASSERT_TRUE(history_.Undo(handle, &error)) << error;
  EXPECT_EQ(BackboneNodeIds(*guard_->document_), before);
  ASSERT_TRUE(history_.Redo(handle, &error)) << error;
  EXPECT_EQ(BackboneNodeIds(*guard_->document_), moved);
}

TEST_F(EditorDocumentHistoryTest, InvalidReconnectLeavesDocumentHashAndHistoryHeadUnchanged) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  ASSERT_TRUE(CommitCapturedAddColorGrade(history_, handle, *guard_->document_,
                                          alcedo::NodeId{"drt"}, alcedo::NodeId{"grade.b"}, &error))
      << error;
  const auto before_hash  = alcedo::CanonicalPipelineDocumentJson(*guard_->document_);
  const auto before_count = guard_->commit_graph_->CommitCount();
  const auto before_head  = guard_->working_head_commit_hash();
  EXPECT_FALSE(CommitCapturedReconnectColorGrade(history_, handle, *guard_->document_,
                                                 alcedo::NodeId{"grade.primary"},
                                                 alcedo::NodeId{"develop"}, alcedo::NodeId{"drt"},
                                                 &error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(alcedo::CanonicalPipelineDocumentJson(*guard_->document_), before_hash);
  EXPECT_EQ(guard_->commit_graph_->CommitCount(), before_count);
  EXPECT_EQ(guard_->working_head_commit_hash(), before_head);
}

TEST_F(EditorDocumentHistoryTest, RenameCreatesHistoryWithoutRenderIntent) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto count = guard_->commit_graph_->CommitCount();
  ASSERT_TRUE(history_.RenameColorGrade(handle, alcedo::NodeId{"grade.primary"}, "Look A", &error))
      << error;
  EXPECT_EQ(guard_->commit_graph_->CommitCount(), count + 1);
  const auto* grade = dynamic_cast<const alcedo::ColorGradeNodeModel*>(
      guard_->document_->Graph().FindNode(alcedo::NodeId{"grade.primary"}));
  ASSERT_NE(grade, nullptr);
  EXPECT_EQ(grade->DisplayName(), "Look A");
  EXPECT_FALSE(history_.LastPublishedRenderReason().has_value());
}

TEST_F(EditorDocumentHistoryTest, NodeDeletionLockHistoryRoundTripHasNoRenderIntent) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto count = guard_->commit_graph_->CommitCount();
  const auto grade_id = alcedo::NodeId{"grade.primary"};
  ASSERT_TRUE(history_.SetColorGradeDeletionProtected(handle, grade_id, false, &error)) << error;
  const auto* grade = dynamic_cast<const alcedo::ColorGradeNodeModel*>(
      guard_->document_->Graph().FindNode(grade_id));
  ASSERT_NE(grade, nullptr);
  EXPECT_FALSE(grade->DeletionProtected());
  EXPECT_EQ(guard_->commit_graph_->CommitCount(), count + 1);
  EXPECT_FALSE(history_.LastPublishedRenderReason().has_value());
  ASSERT_TRUE(history_.SetColorGradeDeletionProtected(handle, grade_id, true, &error)) << error;
  EXPECT_EQ(guard_->commit_graph_->CommitCount(), count + 2);
  EXPECT_FALSE(history_.LastPublishedRenderReason().has_value());
  ASSERT_TRUE(history_.Undo(handle, &error)) << error;
  grade = dynamic_cast<const alcedo::ColorGradeNodeModel*>(
      guard_->document_->Graph().FindNode(grade_id));
  ASSERT_NE(grade, nullptr);
  EXPECT_FALSE(grade->DeletionProtected());
  EXPECT_EQ(guard_->commit_graph_->CommitCount(), count + 2);
  EXPECT_FALSE(history_.LastPublishedRenderReason().has_value());
  ASSERT_TRUE(history_.Redo(handle, &error)) << error;
  grade = dynamic_cast<const alcedo::ColorGradeNodeModel*>(
      guard_->document_->Graph().FindNode(grade_id));
  ASSERT_NE(grade, nullptr);
  EXPECT_TRUE(grade->DeletionProtected());
  EXPECT_EQ(guard_->commit_graph_->CommitCount(), count + 2);
  EXPECT_FALSE(history_.LastPublishedRenderReason().has_value());
}

TEST_F(EditorDocumentHistoryTest, SameValueNodeLockPreservesHistoryAndClearsStaleRenderReason) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto count        = guard_->commit_graph_->CommitCount();
  const auto before_head  = guard_->working_head_commit_hash();
  bool       changed      = true;
  ASSERT_TRUE(history_.SetColorGradeDeletionProtected(
      handle, alcedo::NodeId{"grade.primary"}, true, &error, &changed)) << error;
  EXPECT_FALSE(changed);
  EXPECT_EQ(guard_->commit_graph_->CommitCount(), count);
  EXPECT_EQ(guard_->working_head_commit_hash(), before_head);
  EXPECT_FALSE(history_.LastPublishedRenderReason().has_value());
}

TEST_F(EditorDocumentHistoryTest, MaskCreationUndoRedoRestoresExactIdentity) {
  std::string error;
  const auto handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto grade_id = guard_->document_->DefaultGradeId();
  const MaskId mask_id{"mask.created"};
  ASSERT_TRUE(history_.AddMask(handle, grade_id,
                               grade_mask_test::MakeRadialMask(mask_id), 0, &error)) << error;
  // Masks are never deletion-protected; only the owning Color Grade can lock.
  EXPECT_FALSE(guard_->document_->PrimaryGrade()->FindMask(mask_id)->deletion_protected);
  const auto created = guard_->document_->ToJson();
  ASSERT_TRUE(history_.Undo(handle, &error)) << error;
  EXPECT_EQ(guard_->document_->PrimaryGrade()->FindMask(mask_id), nullptr);
  ASSERT_TRUE(history_.Redo(handle, &error)) << error;
  EXPECT_EQ(guard_->document_->ToJson(), created);
}

TEST_F(EditorDocumentHistoryTest, MixedLockAndPixelBatchRequestsRenderThroughUndoRedo) {
  std::string error;
  const auto handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto grade_id = guard_->document_->DefaultGradeId();
  auto batch = PipelineEditBatch::Make(PipelineEditOperationKind::Paste,
      {SetNodeDeletionProtectionChange{grade_id, true, false},
       SetNodeMixChange{grade_id, 1.0f, 0.5f}}, "history.operation.paste");
  ASSERT_TRUE(history_.CommitPipelineEditBatch(handle, std::move(batch), &error)) << error;
  EXPECT_FALSE(guard_->document_->PrimaryGrade()->DeletionProtected());
  EXPECT_FLOAT_EQ(guard_->document_->PrimaryGrade()->Mix(), 0.5f);
  EXPECT_TRUE(history_.LastPublishedRenderReason().has_value());
  ASSERT_TRUE(history_.Undo(handle, &error)) << error;
  EXPECT_TRUE(guard_->document_->PrimaryGrade()->DeletionProtected());
  EXPECT_FLOAT_EQ(guard_->document_->PrimaryGrade()->Mix(), 1.0f);
  EXPECT_TRUE(history_.LastPublishedRenderReason().has_value());
  ASSERT_TRUE(history_.Redo(handle, &error)) << error;
  EXPECT_FALSE(guard_->document_->PrimaryGrade()->DeletionProtected());
  EXPECT_FLOAT_EQ(guard_->document_->PrimaryGrade()->Mix(), 0.5f);
  EXPECT_TRUE(history_.LastPublishedRenderReason().has_value());
}

TEST_F(EditorDocumentHistoryTest, MultipleMaskRemovalRejectsBeforePartialMutation) {
  std::string error;
  const auto handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto grade_id = guard_->document_->DefaultGradeId();
  const MaskId first{"mask.first"};
  const MaskId second{"mask.second"};
  ASSERT_TRUE(history_.AddMask(handle, grade_id,
                               grade_mask_test::MakeRadialMask(first), 0, &error)) << error;
  ASSERT_TRUE(history_.AddMask(handle, grade_id,
                               grade_mask_test::MakeRadialMask(second), 1, &error)) << error;
  const auto* grade = guard_->document_->PrimaryGrade();
  const auto before = guard_->document_->ToJson();
  const auto head = guard_->working_head_commit_hash();
  const auto count = guard_->commit_graph_->CommitCount();
  // A batch that removes an unknown Mask must reject atomically before any
  // mutation is applied.
  const auto missing_json =
      MaskModelToJson(grade_mask_test::MakeRadialMask(MaskId{"mask.missing"}));
  auto batch = PipelineEditBatch::Make(PipelineEditOperationKind::Paste,
      {RemoveMaskChange{grade_id, first, MaskModelToJson(*grade->FindMask(first)), 0},
       RemoveMaskChange{grade_id, MaskId{"mask.missing"}, missing_json, 0}},
      "history.operation.paste");
  EXPECT_FALSE(history_.CommitPipelineEditBatch(handle, std::move(batch), &error));
  EXPECT_NE(error.find("mask.missing"), std::string::npos);
  EXPECT_EQ(guard_->document_->ToJson(), before);
  EXPECT_EQ(guard_->working_head_commit_hash(), head);
  EXPECT_EQ(guard_->commit_graph_->CommitCount(), count);
}

TEST_F(EditorDocumentHistoryTest, MaskProtectionFlagRoundTripsAcrossUndoAndRedo) {
  // The persisted Mask flag is inert for deletion, but documents and history
  // keep round-tripping it so older projects replay faithfully.
  std::string error;
  const auto handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto grade_id = alcedo::NodeId{"grade.primary"};
  const auto mask_id = alcedo::MaskId{"mask.radial"};
  ASSERT_TRUE(history_.AddMask(handle, grade_id,
                               alcedo::grade_mask_test::MakeRadialMask(mask_id), 0, &error)) << error;
  const auto* grade = dynamic_cast<const alcedo::ColorGradeNodeModel*>(
      guard_->document_->Graph().FindNode(grade_id));
  ASSERT_NE(grade, nullptr);
  const auto revision = grade->MaskContentRevision(mask_id);
  const auto count = guard_->commit_graph_->CommitCount();
  ASSERT_TRUE(history_.LastPublishedRenderReason().has_value());
  EXPECT_FALSE(grade->FindMask(mask_id)->deletion_protected);

  ASSERT_TRUE(history_.SetMaskField(handle, grade_id, mask_id,
                                    "deletion_protected", true, &error)) << error;
  EXPECT_TRUE(grade->FindMask(mask_id)->deletion_protected);
  EXPECT_EQ(grade->MaskContentRevision(mask_id), revision);
  EXPECT_EQ(guard_->commit_graph_->CommitCount(), count + 1);
  EXPECT_FALSE(history_.LastPublishedRenderReason().has_value());
  const auto flagged_head = guard_->working_head_commit_hash();
  // A same-value write is a no-op: no commit, no render reason.
  ASSERT_TRUE(history_.SetMaskField(handle, grade_id, mask_id,
                                    "deletion_protected", true, &error)) << error;
  EXPECT_EQ(guard_->commit_graph_->CommitCount(), count + 1);
  EXPECT_EQ(guard_->working_head_commit_hash(), flagged_head);
  EXPECT_FALSE(history_.LastPublishedRenderReason().has_value());

  ASSERT_TRUE(history_.SetMaskField(handle, grade_id, mask_id,
                                    "deletion_protected", false, &error)) << error;
  EXPECT_FALSE(grade->FindMask(mask_id)->deletion_protected);
  EXPECT_EQ(grade->MaskContentRevision(mask_id), revision);
  EXPECT_EQ(guard_->commit_graph_->CommitCount(), count + 2);
  EXPECT_FALSE(history_.LastPublishedRenderReason().has_value());

  ASSERT_TRUE(history_.Undo(handle, &error)) << error;
  grade = dynamic_cast<const alcedo::ColorGradeNodeModel*>(
      guard_->document_->Graph().FindNode(grade_id));
  ASSERT_NE(grade, nullptr);
  EXPECT_TRUE(grade->FindMask(mask_id)->deletion_protected);
  EXPECT_EQ(grade->MaskContentRevision(mask_id), revision);
  EXPECT_EQ(guard_->commit_graph_->CommitCount(), count + 2);
  EXPECT_FALSE(history_.LastPublishedRenderReason().has_value());
  ASSERT_TRUE(history_.Redo(handle, &error)) << error;
  grade = dynamic_cast<const alcedo::ColorGradeNodeModel*>(
      guard_->document_->Graph().FindNode(grade_id));
  ASSERT_NE(grade, nullptr);
  EXPECT_FALSE(grade->FindMask(mask_id)->deletion_protected);
  EXPECT_EQ(grade->MaskContentRevision(mask_id), revision);
  EXPECT_EQ(guard_->commit_graph_->CommitCount(), count + 2);
  EXPECT_FALSE(history_.LastPublishedRenderReason().has_value());
}

TEST_F(EditorDocumentHistoryTest, AddRenameAndDeleteSnapshotsPresentTypedHistoryTitles) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  ASSERT_TRUE(CommitCapturedAddColorGrade(history_, handle, *guard_->document_,
                                          alcedo::NodeId{"drt"}, alcedo::NodeId{"grade.extra"},
                                          &error))
      << error;
  alcedo::EditorHistorySnapshot added;
  ASSERT_TRUE(history_.ReadHistorySnapshot(handle, &added, &error)) << error;
  ASSERT_FALSE(added.commits.empty());
  const auto add_pres = PresentEditorHistoryCommit(added.commits.front());
  EXPECT_EQ(added.commits.front().presentation_key, "history.operation.add_color_grade");
  EXPECT_EQ(add_pres.display_name.toStdString(), "Add Color Grade");
  EXPECT_EQ(add_pres.after_text.toStdString(), "Color Grade 2");

  ASSERT_TRUE(history_.RenameColorGrade(handle, alcedo::NodeId{"grade.extra"}, "Sky", &error))
      << error;
  alcedo::EditorHistorySnapshot renamed;
  ASSERT_TRUE(history_.ReadHistorySnapshot(handle, &renamed, &error)) << error;
  ASSERT_FALSE(renamed.commits.empty());
  const auto rename_pres = PresentEditorHistoryCommit(renamed.commits.front());
  EXPECT_EQ(renamed.commits.front().presentation_key, "history.operation.rename_color_grade");
  EXPECT_EQ(rename_pres.display_name.toStdString(), "Rename Color Grade");
  EXPECT_EQ(rename_pres.after_text.toStdString(), "Sky");

  ASSERT_TRUE(CommitCapturedRemoveColorGrade(history_, handle, *guard_->document_,
                                             alcedo::NodeId{"grade.extra"}, &error))
      << error;
  alcedo::EditorHistorySnapshot removed;
  ASSERT_TRUE(history_.ReadHistorySnapshot(handle, &removed, &error)) << error;
  ASSERT_FALSE(removed.commits.empty());
  const auto remove_pres = PresentEditorHistoryCommit(removed.commits.front());
  EXPECT_EQ(removed.commits.front().presentation_key, "history.operation.remove_color_grade");
  EXPECT_EQ(remove_pres.display_name.toStdString(), "Delete Color Grade");
  EXPECT_EQ(remove_pres.delta_text.toStdString(), "Sky");
}

TEST_F(EditorDocumentHistoryTest,
       AddJournalFailureRestoresDocumentHeadCounterAndPublishedRenderReason) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto before_document = alcedo::CanonicalPipelineDocumentJson(*guard_->document_);
  const auto before_head     = guard_->working_head_commit_hash();
  const auto before_count    = guard_->commit_graph_->CommitCount();
  const auto before_counter  = guard_->document_->NextColorGradeNameNumber();
  const auto before_reason   = history_.LastPublishedRenderReason();
  ASSERT_TRUE(std::filesystem::create_directory(journal_path_));

  EXPECT_FALSE(CommitCapturedAddColorGrade(history_, handle, *guard_->document_,
                                           alcedo::NodeId{"drt"}, alcedo::NodeId{"grade.failed"},
                                           &error));
  EXPECT_EQ(error, "mini-Git journal file could not be opened for append");
  EXPECT_EQ(alcedo::CanonicalPipelineDocumentJson(*guard_->document_), before_document);
  EXPECT_EQ(guard_->working_head_commit_hash(), before_head);
  EXPECT_EQ(guard_->commit_graph_->CommitCount(), before_count);
  EXPECT_EQ(guard_->document_->NextColorGradeNameNumber(), before_counter);
  EXPECT_EQ(guard_->document_->Graph().FindNode(alcedo::NodeId{"grade.failed"}), nullptr);
  EXPECT_EQ(history_.LastPublishedRenderReason(), before_reason);
}

TEST_F(EditorDocumentHistoryTest, MaskAddRemoveUndoRestoresValueAndDisplayIndex) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto grade_id = alcedo::NodeId{"grade.primary"};
  ASSERT_TRUE(history_.AddMask(handle, grade_id,
                               alcedo::grade_mask_test::MakeRadialMask(
                                   alcedo::MaskId{"mask.first"}),
                               0, &error))
      << error;
  ASSERT_TRUE(history_.AddMask(handle, grade_id,
                               alcedo::grade_mask_test::MakeRadialMask(alcedo::MaskId{"mask.radial"}),
                               1, &error))
      << error;
  ASSERT_TRUE(history_.AddMask(
      handle, grade_id, alcedo::grade_mask_test::MakeLinearGradientMask(alcedo::MaskId{"mask.linear"}),
      2, &error))
      << error;
  const auto* grade = dynamic_cast<const alcedo::ColorGradeNodeModel*>(
      guard_->document_->Graph().FindNode(grade_id));
  ASSERT_NE(grade, nullptr);
  ASSERT_EQ(grade->MaskCount(), 3u);
  EXPECT_EQ(grade->MaskAt(1).id, alcedo::MaskId{"mask.radial"});
  const auto radial_json = alcedo::MaskModelToJson(grade->MaskAt(1));
  ASSERT_TRUE(history_.RemoveMask(handle, grade_id, alcedo::MaskId{"mask.radial"}, &error)) << error;
  EXPECT_EQ(grade->MaskCount(), 2u);
  ASSERT_TRUE(history_.Undo(handle, &error)) << error;
  grade = dynamic_cast<const alcedo::ColorGradeNodeModel*>(
      guard_->document_->Graph().FindNode(grade_id));
  ASSERT_EQ(grade->MaskCount(), 3u);
  EXPECT_EQ(grade->MaskAt(1).id, alcedo::MaskId{"mask.radial"});
  EXPECT_EQ(alcedo::MaskModelToJson(grade->MaskAt(1)).dump(), radial_json.dump());
}

TEST_F(EditorDocumentHistoryTest, MaskSourceUndoRestoresExactVariantValues) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto grade_id = alcedo::NodeId{"grade.primary"};
  alcedo::RadialMaskSource radial;
  radial.center_x     = 0.25f;
  radial.major_radius = 0.4f;
  ASSERT_TRUE(history_.AddMask(handle, grade_id,
                               alcedo::grade_mask_test::MakeRadialMask(alcedo::MaskId{"mask.geo"},
                                                                      radial),
                               0, &error))
      << error;
  auto* grade = dynamic_cast<alcedo::ColorGradeNodeModel*>(
      guard_->document_->Graph().FindNode(grade_id));
  const auto before_source = alcedo::MaskModelToJson(grade->MaskAt(0)).at("source");
  alcedo::LinearGradientMaskSource linear;
  linear.origin_x            = 0.1f;
  linear.transition_distance = 0.35f;
  auto linear_mask = alcedo::grade_mask_test::MakeLinearGradientMask(
      alcedo::MaskId{"mask.geo"}, linear);
  const auto after_source = alcedo::MaskModelToJson(linear_mask).at("source");
  ASSERT_TRUE(history_.ReplaceMaskSource(handle, grade_id, alcedo::MaskId{"mask.geo"}, after_source,
                                         &error))
      << error;
  EXPECT_EQ(alcedo::MaskModelToJson(grade->MaskAt(0)).at("source").dump(), after_source.dump());
  ASSERT_TRUE(history_.Undo(handle, &error)) << error;
  grade = dynamic_cast<alcedo::ColorGradeNodeModel*>(
      guard_->document_->Graph().FindNode(grade_id));
  EXPECT_EQ(alcedo::MaskModelToJson(grade->MaskAt(0)).at("source").dump(), before_source.dump());
}

TEST_F(EditorDocumentHistoryTest, MultiChangeActionCreatesOneCommitAndOneChainFold) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto before_count = guard_->commit_graph_->CommitCount();
  const auto before_chain = guard_->transaction_chain_hash();
  nlohmann::json exposure;
  nlohmann::json contrast;
  ASSERT_TRUE(alcedo::ReadEditorParameterJson(*guard_->document_, ColorGradeFieldTarget("exposure"),
                                              &exposure, &error))
      << error;
  ASSERT_TRUE(alcedo::ReadEditorParameterJson(*guard_->document_, ColorGradeFieldTarget("contrast"),
                                              &contrast, &error))
      << error;
  alcedo::SetParameterChange first;
  first.target         = alcedo::ToPipelineParameterTarget(ColorGradeFieldTarget("exposure"));
  first.before_value   = exposure;
  first.after_value    = exposure;
  first.after_value["exposure_ev"] = 3.0;
  first.before_enabled = true;
  first.after_enabled  = true;
  alcedo::SetParameterChange second;
  second.target         = alcedo::ToPipelineParameterTarget(ColorGradeFieldTarget("contrast"));
  second.before_value   = contrast;
  second.after_value    = contrast;
  second.after_value["contrast"] = 0.3;
  second.before_enabled = true;
  second.after_enabled  = true;
  auto batch = alcedo::PipelineEditBatch::Make(
      alcedo::PipelineEditOperationKind::Paste, {first, second},
      alcedo::PresentationKeyForOperation(alcedo::PipelineEditOperationKind::Paste));
  ASSERT_TRUE(history_.CommitPipelineEditBatch(handle, batch, &error)) << error;
  EXPECT_EQ(guard_->commit_graph_->CommitCount(), before_count + 1);
  EXPECT_NE(guard_->transaction_chain_hash(), before_chain);
  EXPECT_EQ(batch.changes.size(), 2u);
}

TEST_F(EditorDocumentHistoryTest, UndoRedoAppliesTypedBatchesInRequiredOrder) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  auto first = WithColorGradeTarget({"exposure", R"({"exposure_ev":2.0})", true});
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, first, &error)) << error;
  ASSERT_TRUE(history_.CommitAdjustment(handle, first, &error)) << error;
  auto second = WithColorGradeTarget({"exposure", R"({"exposure_ev":3.0})", true});
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, second, &error)) << error;
  ASSERT_TRUE(history_.CommitAdjustment(handle, second, &error)) << error;
  EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_, "grade.primary"), 3.0f);
  ASSERT_TRUE(history_.Undo(handle, &error)) << error;
  EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_, "grade.primary"), 2.0f);
  ASSERT_TRUE(history_.Undo(handle, &error)) << error;
  EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_, "grade.primary"), 1.5f);
  ASSERT_TRUE(history_.Redo(handle, &error)) << error;
  EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_, "grade.primary"), 2.0f);
  ASSERT_TRUE(history_.Redo(handle, &error)) << error;
  EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_, "grade.primary"), 3.0f);
}

TEST_F(EditorDocumentHistoryTest, MoveToAncestorAndRedoChildUsesStoredDirections) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto root_hash = alcedo::CanonicalPipelineDocumentJson(*guard_->document_);
  auto first = WithColorGradeTarget({"exposure", R"({"exposure_ev":2.0})", true});
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, first, &error)) << error;
  ASSERT_TRUE(history_.CommitAdjustment(handle, first, &error)) << error;
  const auto first_head = *guard_->working_head_commit_hash();
  const auto first_hash = alcedo::CanonicalPipelineDocumentJson(*guard_->document_);
  auto second = WithColorGradeTarget({"exposure", R"({"exposure_ev":3.0})", true});
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, second, &error)) << error;
  ASSERT_TRUE(history_.CommitAdjustment(handle, second, &error)) << error;
  const auto second_hash = alcedo::CanonicalPipelineDocumentJson(*guard_->document_);
  ASSERT_TRUE(history_.MoveHeadToCommit(handle, first_head, &error)) << error;
  EXPECT_EQ(alcedo::CanonicalPipelineDocumentJson(*guard_->document_), first_hash);
  ASSERT_TRUE(history_.Redo(handle, &error)) << error;
  EXPECT_EQ(alcedo::CanonicalPipelineDocumentJson(*guard_->document_), second_hash);
  ASSERT_TRUE(history_.Undo(handle, &error)) << error;
  ASSERT_TRUE(history_.Undo(handle, &error)) << error;
  EXPECT_EQ(alcedo::CanonicalPipelineDocumentJson(*guard_->document_), root_hash);
}

}  // namespace
}  // namespace alcedo::ui
