//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The editor history publishes a committed pipeline graph snapshot of the image it holds after
// every change of its committed state, and never while an input sequence has left uncommitted
// values on the live document. Thumbnails and analysis read these snapshots through
// PipelineMgmtService::AcquireCommittedSnapshot.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>

#include "app/editor_pipeline_command_service.hpp"
#include "app/pipeline_document_history.hpp"
#include "app/pipeline_service.hpp"
#include "app/project_service.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_snapshot.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "edit/mask/mask_model.hpp"
#include "grade_owned_mask_support.hpp"
#include "json.hpp"
#include "support/editor_parameter_target_test.hpp"
#include "support/editor_parameter_write_test.hpp"
#include "ui/alcedo_main/album_backend/editor_session_history_port.hpp"
#include "ui/alcedo_main/album_backend/editor_session_pipeline_port.hpp"
#include "utils/clock/time_provider.hpp"

namespace alcedo::ui {
namespace {

using alcedo::test::WithColorGradeTarget;

constexpr sl_element_id_t kElementId = 931;

/// Exposure of the current panel read the way the editor reads it; NaN when unavailable.
auto                      ExposureEv(const alcedo::PipelineDocument& document) -> double {
  std::string    error;
  const auto     target = alcedo::CompleteCurrentPanelParameterTarget(document, "exposure", &error);
  nlohmann::json json;
  if (!target.has_value() || !alcedo::ReadEditorParameterJson(document, *target, &json, &error) ||
      !json.contains("exposure_ev")) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return json.at("exposure_ev").get<double>();
}

class EditorCommittedSnapshotPublicationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    TimeProvider::Refresh();
    const auto stamp =
        std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const auto base = std::filesystem::temp_directory_path();
    db_path_        = base / ("committed_publication_" + stamp + ".db");
    meta_path_      = base / ("committed_publication_" + stamp + ".json");
    journal_path_   = base / ("committed_publication_" + stamp + ".wal");
    project_        = std::make_unique<alcedo::ProjectService>(db_path_, meta_path_,
                                                               alcedo::ProjectOpenMode::kCreateNew);
    pipelines_      = std::make_shared<alcedo::PipelineMgmtService>(project_->GetStorage());
    pipeline_port_  = std::make_shared<EditorSessionPipelinePort>();
    pipeline_port_->SetServices(
        EditorSessionPipelineMappers{[pipelines = pipelines_]() { return pipelines; },
                                     [pipelines = pipelines_](sl_element_id_t id) {
                                       return pipelines->AcquireEditorPipeline(id);
                                     }});
    history_.SetServices(
        EditorSessionHistoryPort::Services{[this](sl_element_id_t) { return journal_path_; }});
    history_.SetPipelinePort(pipeline_port_);
    std::string error;
    handle_ = history_.Acquire(kElementId, &error);
    ASSERT_TRUE(handle_.valid) << error;
    guard_ = pipeline_port_->CurrentGuard(kElementId);
    ASSERT_NE(guard_, nullptr);
  }

  void TearDown() override {
    history_.Release(handle_);
    pipeline_port_->Release({kElementId, true});
    guard_.reset();
    pipelines_.reset();
    project_.reset();
    std::error_code ec;
    std::filesystem::remove(db_path_, ec);
    std::filesystem::remove(meta_path_, ec);
    std::filesystem::remove(journal_path_, ec);
  }

  auto Committed() -> std::shared_ptr<const alcedo::PipelineGraphSnapshot> {
    return pipelines_->AcquireCommittedSnapshot(kElementId);
  }

  std::filesystem::path                        db_path_;
  std::filesystem::path                        meta_path_;
  std::filesystem::path                        journal_path_;
  std::unique_ptr<alcedo::ProjectService>      project_;
  std::shared_ptr<alcedo::PipelineMgmtService> pipelines_;
  std::shared_ptr<EditorSessionPipelinePort>   pipeline_port_;
  EditorSessionHistoryPort                     history_;
  alcedo::EditorHistoryGuardHandle             handle_{};
  std::shared_ptr<alcedo::PipelineGuard>       guard_;
};

TEST_F(EditorCommittedSnapshotPublicationTest, OpeningTheImagePublishesItsCommittedState) {
  const auto committed = Committed();
  ASSERT_NE(committed, nullptr);
  EXPECT_TRUE(committed->IsCommitted());
  EXPECT_EQ(committed->ElementId(), kElementId);
  EXPECT_EQ(committed->Head(), guard_->working_head_commit_hash());
  EXPECT_EQ(committed->Chain(), guard_->transaction_chain_hash());
  EXPECT_EQ(committed->Lineage(), guard_->lineage_);
  EXPECT_EQ(committed->Document().ToJson(), guard_->document_->ToJson());
  EXPECT_EQ(pipelines_->CommittedSnapshotStorageLoadCount(), 0u)
      << "the image the editor holds is served from the editor's publication";
}

TEST_F(EditorCommittedSnapshotPublicationTest, SliderPreviewIsNotPublishedUntilItSettles) {
  std::string error;
  const auto  before = Committed();
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(
      handle_, WithColorGradeTarget({"exposure", R"({"exposure":0.25})", false}), &error))
      << error;
  EXPECT_TRUE(guard_->unsettled_preview_);
  EXPECT_EQ(Committed(), before) << "a preview value is not a committed state";
  EXPECT_DOUBLE_EQ(ExposureEv(Committed()->Document()), ExposureEv(before->Document()));

  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(
      handle_, WithColorGradeTarget({"exposure", R"({"exposure":0.75})", false}), &error))
      << error;
  EXPECT_EQ(Committed(), before);
  ASSERT_TRUE(history_.CommitAdjustment(
      handle_, WithColorGradeTarget({"exposure", R"({"exposure":0.75})", true}), &error))
      << error;
  EXPECT_FALSE(guard_->unsettled_preview_);
  const auto settled = Committed();
  ASSERT_NE(settled, before);
  ASSERT_TRUE(settled->Head().has_value());
  EXPECT_EQ(settled->Head(), guard_->working_head_commit_hash());
  EXPECT_EQ(settled->Chain(), guard_->transaction_chain_hash());
  EXPECT_NEAR(ExposureEv(settled->Document()), 0.75, 1e-6);

  ASSERT_TRUE(history_.Undo(handle_, &error)) << error;
  const auto undone = Committed();
  EXPECT_EQ(undone->Head(), before->Head());
  EXPECT_EQ(undone->Chain(), before->Chain());
  EXPECT_DOUBLE_EQ(ExposureEv(undone->Document()), ExposureEv(before->Document()));
  EXPECT_EQ(pipelines_->CommittedSnapshotStorageLoadCount(), 0u);
}

TEST_F(EditorCommittedSnapshotPublicationTest, CancelledPreviewLeavesThePublishedStateUnchanged) {
  std::string error;
  const auto  before = Committed();
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(
      handle_, WithColorGradeTarget({"exposure", R"({"exposure":1.5})", false}), &error))
      << error;
  bool live_changed = false;
  ASSERT_TRUE(history_.RestoreUnsettledPreview(handle_, &live_changed, &error)) << error;
  EXPECT_TRUE(live_changed);
  EXPECT_FALSE(guard_->unsettled_preview_);
  EXPECT_EQ(Committed(), before) << "restoring the committed values publishes nothing new";
}

TEST_F(EditorCommittedSnapshotPublicationTest, OpenMaskInputIsUncommittedUntilItsSettle) {
  std::string  error;
  const auto   before   = Committed();
  const auto   grade_id = guard_->document_->DefaultGradeId();
  const MaskId mask_id{"mask.drag"};

  // First call: the input sequence writes a provisional mask and stays open, as a Mask drag does
  // between pointer samples.
  ASSERT_TRUE(history_.WithLockedLiveDocument(
      handle_,
      [&](alcedo::PipelineDocument& document, alcedo::MiniGitWorkingHistory&,
          const alcedo::IEditorHistoryPort::LockedMaskSettle&, bool* input_open, std::string*) {
        auto* grade =
            dynamic_cast<alcedo::ColorGradeNodeModel*>(document.Graph().FindNode(grade_id));
        grade->AddMask(alcedo::grade_mask_test::MakeRadialMask(mask_id), 0);
        *input_open = true;
        return true;
      },
      &error))
      << error;
  EXPECT_TRUE(guard_->unsettled_preview_)
      << "an open Mask input is uncommitted exactly like a slider preview";
  EXPECT_EQ(Committed(), before);
  EXPECT_EQ(Committed()->Document().PrimaryGrade()->FindMask(mask_id), nullptr);

  // Second call: the pointer release settles the provisional mask as one commit and closes.
  ASSERT_TRUE(history_.WithLockedLiveDocument(
      handle_,
      [&](alcedo::PipelineDocument& document, alcedo::MiniGitWorkingHistory&,
          const alcedo::IEditorHistoryPort::LockedMaskSettle& settle, bool* input_open,
          std::string* op_error) {
        const auto* grade = document.PrimaryGrade();
        const auto  batch = alcedo::MakeAddMaskBatch(
            grade_id, mask_id, alcedo::MaskModelToJson(*grade->FindMask(mask_id)), 0);
        const bool settled = settle(batch, op_error);
        *input_open        = false;
        return settled;
      },
      &error))
      << error;
  EXPECT_FALSE(guard_->unsettled_preview_);
  const auto settled = Committed();
  ASSERT_NE(settled, before);
  EXPECT_EQ(settled->Head(), guard_->working_head_commit_hash());
  EXPECT_NE(settled->Document().PrimaryGrade()->FindMask(mask_id), nullptr);
}

TEST_F(EditorCommittedSnapshotPublicationTest, ReleasedImageIsServedFromStorageWhenItDiffers) {
  std::string error;
  const auto  opened = Committed();
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(
      handle_, WithColorGradeTarget({"exposure", R"({"exposure":0.25})", false}), &error))
      << error;
  ASSERT_TRUE(history_.CommitAdjustment(
      handle_, WithColorGradeTarget({"exposure", R"({"exposure":0.75})", true}), &error))
      << error;
  const auto journaled = Committed();
  ASSERT_TRUE(journaled->Head().has_value());

  // Release without a save: the commit exists only in the journal, so storage still holds the
  // root state and the released entry no longer matches it.
  history_.Release(handle_);
  pipeline_port_->Release({kElementId, true});
  handle_           = {};
  const auto stored = Committed();
  EXPECT_FALSE(stored->Head().has_value());
  EXPECT_EQ(pipelines_->CommittedSnapshotStorageLoadCount(), 1u);
  EXPECT_EQ(stored->Chain(), opened->Chain());
  EXPECT_DOUBLE_EQ(ExposureEv(stored->Document()), ExposureEv(opened->Document()));
}

}  // namespace
}  // namespace alcedo::ui
