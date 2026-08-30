//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>

#include "app/editor_pipeline_command_service.hpp"
#include "app/pipeline_service.hpp"
#include "edit/graph/pipeline_graph_commands.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "support/editor_parameter_target_test.hpp"
#include "ui/alcedo_main/album_backend/editor_session_history_port.hpp"

namespace alcedo::ui {
namespace {

using test::ColorGradeFieldTarget;
using test::WithColorGradeTarget;

/// Real document, executor, history and WAL; no project/storage/UI fixture is needed.
class EditorDocumentHistoryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RegisterAllOperators();
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
  alcedo::EditorAdjustmentPatch patch;
  patch.field_key   = "exposure";
  patch.params_json = R"({"exposure_ev":3.0})";
  EXPECT_FALSE(history_.CaptureAdjustmentBeforePreview(handle, patch, &error));
  EXPECT_EQ(error, "Editor parameter target requires owner_kind");
  EXPECT_EQ(alcedo::CanonicalPipelineDocumentJson(*guard_->document_), before_hash);
  EXPECT_FALSE(guard_->working_head_commit_hash().has_value());
  EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_, "grade.primary"),
                  alcedo::kDefaultPipelineExposureEv);

  auto missing_node           = WithColorGradeTarget({"exposure", R"({"exposure_ev":3.0})", false});
  missing_node.target.node_id = alcedo::NodeId{};
  EXPECT_FALSE(history_.CaptureAdjustmentBeforePreview(handle, missing_node, &error));
  EXPECT_EQ(error, "Editor parameter target requires node_id");
  EXPECT_EQ(alcedo::CanonicalPipelineDocumentJson(*guard_->document_), before_hash);
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

  alcedo::EditorAdjustmentPatch incomplete;
  incomplete.field_key   = "exposure";
  incomplete.params_json = R"({"exposure_ev":4.0})";
  EXPECT_FALSE(history_.CaptureAdjustmentBeforePreview(handle, incomplete, &error));
  EXPECT_EQ(error, "Editor parameter target requires owner_kind");
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
  alcedo::EditorAdjustmentPatch patch;
  patch.field_key                     = "exposure";
  patch.params_json                   = R"({"exposure_ev":3.0})";
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
  const auto stages = guard_->pipeline_->ExportPipelineParams();
  auto valid = WithColorGradeTarget({"exposure", R"({"exposure_ev":100.0})", true}, "grade.extra");
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, valid, &error)) << error;
  ASSERT_TRUE(history_.CommitAdjustment(handle, valid, &error)) << error;
  EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_, "grade.primary"), 1.5f);
  EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_, "grade.extra"), 16.0f);
  const auto head = *guard_->working_head_commit_hash();
  const auto payload =
      OrdinaryEditPayload::FromJSON(guard_->commit_graph_->GetCommit(head).GetPayloadJSON());
  EXPECT_EQ(payload.before_value.at("exposure"), 0.0);
  EXPECT_EQ(payload.after_value.at("exposure"), 16.0);
  // A different raw request normalizes to the same value, so it cannot produce another commit.
  valid.params_json = R"({"exposure_ev":20.0})";
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, valid, &error)) << error;
  ASSERT_TRUE(history_.CommitAdjustment(handle, valid, &error)) << error;
  EXPECT_EQ(guard_->working_head_commit_hash(), head);
  ASSERT_TRUE(history_.Undo(handle, &error)) << error;
  EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_, "grade.extra"), 0.0f);
  ASSERT_TRUE(history_.Redo(handle, &error)) << error;
  EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_, "grade.extra"), 16.0f);
  EXPECT_EQ(guard_->pipeline_->ExportPipelineParams(), stages);
}

TEST_F(EditorDocumentHistoryTest, PreviewCommitUndoRedoAndCancelWaitForRenderLock) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto* grade       = guard_->document_->PrimaryGrade();
  const auto* exposure    = grade->FindAdjustmentByType(type_ids::Exposure());
  const auto  patch       = WithColorGradeTarget({"exposure", R"({"exposure_ev":2.5})", true});
  const auto  stages      = guard_->pipeline_->ExportPipelineParams();
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
  EXPECT_EQ(guard_->pipeline_->ExportPipelineParams(), stages);
}

TEST_F(EditorDocumentHistoryTest, UnrecordedHistoryTargetIsRejectedBeforeJournalOrDocumentChanges) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  auto patch = WithColorGradeTarget({"exposure", R"({"exposure_ev":2.5})", true});
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, patch, &error)) << error;
  ASSERT_TRUE(history_.CommitAdjustment(handle, patch, &error)) << error;
  // Releasing this session loses local targets. No stage-based replay is permitted in NM1.4 A.
  history_.Release(handle);
  const auto reopened = history_.Acquire(42, &error);
  ASSERT_TRUE(reopened.valid) << error;
  const auto before = guard_->document_->ToJson();
  const auto head   = guard_->working_head_commit_hash();
  EXPECT_FALSE(history_.Undo(reopened, &error));
  EXPECT_NE(error.find("same-session document target"), std::string::npos);
  EXPECT_EQ(guard_->working_head_commit_hash(), head);
  EXPECT_EQ(guard_->document_->ToJson(), before);
}

}  // namespace
}  // namespace alcedo::ui
