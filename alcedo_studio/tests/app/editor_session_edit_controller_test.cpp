//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_session_edit_controller.hpp"
#include "app/editor_pending_input.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "app/editor_session_lifecycle.hpp"
#include "edit/operators/op_base.hpp"
#include "json.hpp"
#include "support/editor_parameter_target_test.hpp"
#include "support/editor_parameter_write_test.hpp"
#include "support/editor_session_test_ports.hpp"

namespace alcedo {
namespace {

class EditorSessionEditControllerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    pipeline_ = std::make_shared<test::FakeEditorPipelinePort>();
    history_  = std::make_shared<test::FakeEditorHistoryPort>();

    EditorSessionLifecycle::Dependencies life_deps;
    life_deps.pipeline = pipeline_;
    life_deps.history  = history_;
    lifecycle_         = std::make_unique<EditorSessionLifecycle>(std::move(life_deps));

    std::string error;
    ASSERT_TRUE(lifecycle_->BeginAcquire(1, 2, false, nullptr, &error)) << error;
    ASSERT_TRUE(lifecycle_->AcquireGuards(&error)) << error;
    lifecycle_->MarkImageReady();
    lifecycle_->MarkFirstFrameReady();

    EditorSessionEditController::Dependencies edit_deps{history_};
    edit_ = std::make_unique<EditorSessionEditController>(std::move(edit_deps));
  }

  auto guard() const -> EditorHistoryGuardHandle { return lifecycle_->history_guard(); }
  auto identity() const -> EditorSessionIdentity { return lifecycle_->identity(); }

  std::shared_ptr<test::FakeEditorPipelinePort> pipeline_;
  std::shared_ptr<test::FakeEditorHistoryPort>  history_;
  std::unique_ptr<EditorSessionLifecycle>       lifecycle_;
  std::unique_ptr<EditorSessionEditController>  edit_;
};

TEST_F(EditorSessionEditControllerTest, InteractiveAndSettledPatchUseOneHistoryCommit) {
  auto patch = test::WithColorGradeTarget(test::ScalarPatch("exposure", 1.0f, false));

  auto r1 = edit_->HandlePatch(patch, false, guard(), identity());
  EXPECT_EQ(r1.kind, EditorEditOutcome::Kind::RenderRouted);
  EXPECT_EQ(r1.reason, EditorRenderReason::InteractiveAdjustment);
  EXPECT_EQ(history_->capture_count, 1);
  EXPECT_EQ(history_->commit_count, 0);
  EXPECT_EQ(r1.render_command.reason, EditorRenderReason::InteractiveAdjustment);

  patch.settled = true;
  auto r2       = edit_->HandlePatch(patch, true, guard(), identity());
  EXPECT_EQ(r2.kind, EditorEditOutcome::Kind::RenderRouted);
  EXPECT_EQ(r2.reason, EditorRenderReason::SettledAdjustment);
  EXPECT_EQ(history_->capture_count, 2);
  EXPECT_EQ(history_->commit_count, 1);
  EXPECT_EQ(history_->last_committed_patch.field_key, "exposure");
  EXPECT_TRUE(history_->last_committed_patch.settled);
  EXPECT_EQ(r2.render_command.reason, EditorRenderReason::SettledAdjustment);
}

// The typed write goes to history, which writes the document. The render command names only
// the render reason; the renderer reads the bound document.
TEST_F(EditorSessionEditControllerTest, InteractivePatchSendsTypedWriteToHistoryOnly) {
  auto patch = test::WithColorGradeTarget(test::ScalarPatch("exposure", 1.25f, false));
  const auto result = edit_->HandlePatch(patch, false, guard(), identity());
  ASSERT_EQ(result.kind, EditorEditOutcome::Kind::RenderRouted);
  EXPECT_EQ(result.render_command.reason, EditorRenderReason::InteractiveAdjustment);
  EXPECT_EQ(history_->last_captured_patch.field_key, "exposure");
  ASSERT_TRUE(history_->last_captured_patch.write.has_value());
  EXPECT_EQ(test::ScalarValue(*history_->last_captured_patch.write), 1.25f);
}

TEST_F(EditorSessionEditControllerTest, SettledCommitFailureReturnsRejected) {
  history_->fail_commit = true;
  auto patch = test::WithColorGradeTarget(test::ScalarPatch("exposure", 0.5f, true));
  auto                  result = edit_->HandlePatch(patch, true, guard(), identity());
  EXPECT_EQ(result.kind, EditorEditOutcome::Kind::Rejected);
  EXPECT_EQ(result.message, "mini-Git journal append failed");
  EXPECT_EQ(history_->commit_count, 1);
}

TEST_F(EditorSessionEditControllerTest, RepeatedInteractivePatchesCommitOnlyTheSettledValue) {
  auto patch = test::WithColorGradeTarget(test::ScalarPatch("exposure", 0.0f, false));
  for (int value = 0; value < 50; ++value) {
    patch.write = EditorScalarWrite{static_cast<float>(value)};
    const auto routed = edit_->HandlePatch(patch, false, guard(), identity());
    ASSERT_EQ(routed.kind, EditorEditOutcome::Kind::RenderRouted);
    EXPECT_EQ(routed.render_command.reason, EditorRenderReason::InteractiveAdjustment);
  }
  EXPECT_EQ(history_->capture_count, 50);
  EXPECT_EQ(history_->commit_count, 0);

  patch.settled = true;
  const auto settled = edit_->HandlePatch(patch, true, guard(), identity());
  EXPECT_EQ(settled.render_command.reason, EditorRenderReason::SettledAdjustment);
  ASSERT_TRUE(history_->last_committed_patch.write.has_value());
  EXPECT_EQ(test::ScalarValue(*history_->last_committed_patch.write), 49.0f);
  EXPECT_EQ(history_->commit_count, 1);
}

TEST_F(EditorSessionEditControllerTest, UndoMovesHistoryWithoutParameterWrite) {
  auto result = edit_->HandleUndoRedo(true, guard(), identity());
  EXPECT_EQ(result.kind, EditorEditOutcome::Kind::Accepted);
  EXPECT_EQ(history_->undo_count, 1);
  EXPECT_EQ(history_->capture_count, 0);
  EXPECT_EQ(history_->commit_count, 0);
}

TEST_F(EditorSessionEditControllerTest, UndoFailureReturnsFailed) {
  history_->fail_undo = true;
  auto result         = edit_->HandleUndoRedo(true, guard(), identity());
  EXPECT_EQ(result.kind, EditorEditOutcome::Kind::Failed);
  EXPECT_EQ(result.message, "undo failed");
}

TEST_F(EditorSessionEditControllerTest, DiscardUsesHistoryPortWithoutParameterWrite) {
  auto result = edit_->HandleDiscard(guard(), identity(), EditorSessionState::Interactive);
  EXPECT_EQ(result.kind, EditorEditOutcome::Kind::Accepted);
  EXPECT_EQ(history_->discard_count, 1);
  EXPECT_EQ(history_->capture_count, 0);
}

TEST_F(EditorSessionEditControllerTest, PatchWithEmptyFieldKeyIsRejected) {
  EditorAdjustmentPatch patch;
  patch.write = EditorScalarWrite{0.0f};
  auto        result = edit_->HandlePatch(patch, false, guard(), identity());
  EXPECT_EQ(result.kind, EditorEditOutcome::Kind::Rejected);
}

TEST_F(EditorSessionEditControllerTest, PatchWithoutValidGuardIsRejected) {
  EditorHistoryGuardHandle invalid_guard{};
  auto patch = test::WithColorGradeTarget(test::ScalarPatch("exposure", 1.0f, false));
  auto result = edit_->HandlePatch(patch, false, invalid_guard, identity());
  EXPECT_EQ(result.kind, EditorEditOutcome::Kind::Rejected);
}

TEST_F(EditorSessionEditControllerTest, UnspecifiedFieldKeyIsRoutedToHistory) {
  EditorAdjustmentPatch patch = test::ScalarPatch("exposure", 1.0f, false);
  auto result       = edit_->HandlePatch(patch, false, guard(), identity());
  EXPECT_EQ(result.kind, EditorEditOutcome::Kind::RenderRouted);
  EXPECT_EQ(history_->capture_count, 1);
}

TEST_F(EditorSessionEditControllerTest, ExplicitIncompleteColorGradeTargetIsRejected) {
  auto patch = test::WithColorGradeTarget(test::ScalarPatch("exposure", 1.0f, false));
  patch.target.node_id = NodeId{};
  auto result          = edit_->HandlePatch(patch, false, guard(), identity());
  EXPECT_EQ(result.kind, EditorEditOutcome::Kind::Rejected);
  EXPECT_EQ(result.message, "Editor parameter target requires node_id");
  EXPECT_EQ(history_->capture_count, 0);
}

TEST_F(EditorSessionEditControllerTest, MaskTargetIsRejected) {
  EditorAdjustmentPatch patch = test::ScalarPatch("exposure", 1.0f, false);
  patch.target.owner_kind              = EditorParameterOwnerKind::ColorGradeMask;
  patch.target.node_id                 = NodeId{"grade.primary"};
  patch.target.adjustment_instance_id  = AdjustmentInstanceId{"grade.primary.exposure"};
  patch.target.mask_id                 = "mask.1";
  patch.target.field_key               = "exposure";
  auto result                          = edit_->HandlePatch(patch, false, guard(), identity());
  EXPECT_EQ(result.kind, EditorEditOutcome::Kind::Rejected);
  EXPECT_EQ(result.message, "Mask parameter targets are rejected until NM3");
  EXPECT_EQ(history_->capture_count, 0);
}

TEST_F(EditorSessionEditControllerTest, PendingSequenceAppliesEveryFieldOnceThenCommitsRelease) {
  EditorPendingSequence sequence;
  sequence.seal = EditorPendingInputBoundaryKind::Release;
  EditorPendingFieldChange exposure;
  exposure.target.field_key = "exposure";
  exposure.write            = EditorScalarWrite{0.8f};
  EditorPendingFieldChange contrast;
  contrast.target.field_key = "contrast";
  contrast.write            = EditorScalarWrite{12.0f};
  sequence.fields           = {exposure, contrast};

  const auto result = edit_->HandlePendingSequence(sequence, guard(), identity());
  EXPECT_EQ(result.kind, EditorEditOutcome::Kind::RenderRouted);
  EXPECT_EQ(result.reason, EditorRenderReason::SettledAdjustment);
  EXPECT_EQ(history_->capture_count, 2);
  EXPECT_EQ(history_->commit_count, 2);
  EXPECT_EQ(history_->last_committed_patch.field_key, "contrast");
  EXPECT_EQ(result.render_command.reason, EditorRenderReason::SettledAdjustment);
}

TEST_F(EditorSessionEditControllerTest, InteractiveSequenceCapturesWithoutCommit) {
  EditorPendingSequence sequence;
  sequence.seal = EditorPendingInputBoundaryKind::None;
  EditorPendingFieldChange exposure;
  exposure.target.field_key = "exposure";
  exposure.write            = EditorScalarWrite{0.4f};
  sequence.fields           = {exposure};

  const auto result = edit_->HandlePendingSequence(sequence, guard(), identity());
  EXPECT_EQ(result.kind, EditorEditOutcome::Kind::RenderRouted);
  EXPECT_EQ(result.reason, EditorRenderReason::InteractiveAdjustment);
  EXPECT_EQ(history_->capture_count, 1);
  EXPECT_EQ(history_->commit_count, 0);
  EXPECT_EQ(result.render_command.reason, EditorRenderReason::InteractiveAdjustment);
}

TEST_F(EditorSessionEditControllerTest, CancelRestoreRoutesRenderOnlyWhenLiveContentChanged) {
  history_->restore_changes_live = false;
  EditorPendingSequence empty_cancel;
  empty_cancel.seal = EditorPendingInputBoundaryKind::Cancel;
  auto skipped      = edit_->HandlePendingSequence(empty_cancel, guard(), identity());
  EXPECT_EQ(skipped.kind, EditorEditOutcome::Kind::Accepted);
  EXPECT_FALSE(skipped.schedule_render);
  EXPECT_EQ(history_->restore_preview_count, 1);

  history_->restore_changes_live = true;
  auto restored = edit_->HandlePendingSequence(empty_cancel, guard(), identity());
  EXPECT_EQ(restored.kind, EditorEditOutcome::Kind::RenderRouted);
  EXPECT_EQ(restored.render_command.reason, EditorRenderReason::InteractiveAdjustment);
  EXPECT_EQ(history_->restore_preview_count, 2);
}

}  // namespace
}  // namespace alcedo
