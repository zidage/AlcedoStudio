//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_session_edit_controller.hpp"
#include "app/editor_pending_input.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "app/editor_session_lifecycle.hpp"
#include "edit/history/edit_transaction.hpp"
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
    journal_  = std::make_shared<test::FakeEditorJournalPort>();

    EditorSessionLifecycle::Dependencies life_deps;
    life_deps.pipeline = pipeline_;
    life_deps.history  = history_;
    lifecycle_         = std::make_unique<EditorSessionLifecycle>(std::move(life_deps));

    std::string error;
    ASSERT_TRUE(lifecycle_->BeginAcquire(1, 2, false, nullptr, &error)) << error;
    ASSERT_TRUE(lifecycle_->AcquireGuards(&error)) << error;
    lifecycle_->MarkImageReady();
    lifecycle_->MarkFirstFrameReady();

    EditorSessionEditController::Dependencies edit_deps{history_, journal_};
    edit_ = std::make_unique<EditorSessionEditController>(std::move(edit_deps));
  }

  auto guard() const -> EditorHistoryGuardHandle { return lifecycle_->history_guard(); }
  auto identity() const -> EditorSessionIdentity { return lifecycle_->identity(); }

  std::shared_ptr<test::FakeEditorPipelinePort> pipeline_;
  std::shared_ptr<test::FakeEditorHistoryPort>  history_;
  std::shared_ptr<test::FakeEditorJournalPort>  journal_;
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
  ASSERT_EQ(r1.render_command.adjustment.patches.size(), 1u);
  EXPECT_EQ(r1.render_command.adjustment.patches.front().field_key, "exposure");
  EXPECT_EQ(r1.render_command.adjustment.fingerprint, "exposure");

  patch.settled = true;
  auto r2       = edit_->HandlePatch(patch, true, guard(), identity());
  EXPECT_EQ(r2.kind, EditorEditOutcome::Kind::RenderRouted);
  EXPECT_EQ(r2.reason, EditorRenderReason::SettledAdjustment);
  EXPECT_EQ(history_->capture_count, 2);
  EXPECT_EQ(history_->commit_count, 1);
  EXPECT_EQ(history_->last_committed_patch.field_key, "exposure");
  EXPECT_TRUE(history_->last_committed_patch.settled);
  ASSERT_EQ(r2.render_command.adjustment.patches.size(), 1u);
  EXPECT_EQ(r2.render_command.adjustment.patches.front().field_key, "exposure");
}

TEST_F(EditorSessionEditControllerTest, InteractivePatchCarriesOnlyEditedField) {
  auto patch = test::WithColorGradeTarget(test::ScalarPatch("exposure", 1.25f, false));
  const auto result = edit_->HandlePatch(patch, false, guard(), identity());
  ASSERT_EQ(result.kind, EditorEditOutcome::Kind::RenderRouted);
  ASSERT_EQ(result.render_command.adjustment.patches.size(), 1u);
  EXPECT_EQ(result.render_command.adjustment.patches.front().field_key, "exposure");
  ASSERT_TRUE(result.render_command.adjustment.patches.front().write.has_value());
  EXPECT_EQ(test::ScalarValue(*result.render_command.adjustment.patches.front().write), 1.25f);
  EXPECT_TRUE(result.render_command.adjustment.patches.front().params_json.empty());
  EXPECT_EQ(result.render_command.adjustment.fingerprint, "exposure");
}

TEST_F(EditorSessionEditControllerTest, SettledCommitFailureReturnsRejected) {
  history_->fail_commit = true;
  auto patch = test::WithColorGradeTarget(test::ScalarPatch("exposure", 0.5f, true));
  auto                  result = edit_->HandlePatch(patch, true, guard(), identity());
  EXPECT_EQ(result.kind, EditorEditOutcome::Kind::Rejected);
  EXPECT_EQ(result.message, "mini-Git journal append failed");
  EXPECT_EQ(history_->commit_count, 1);
}

TEST_F(EditorSessionEditControllerTest, RepeatedInteractivePatchesOnlyStampLatestFieldOnRender) {
  auto patch = test::WithColorGradeTarget(test::ScalarPatch("exposure", 0.0f, false));
  for (int value = 0; value < 50; ++value) {
    patch.write = EditorScalarWrite{static_cast<float>(value)};
    const auto routed = edit_->HandlePatch(patch, false, guard(), identity());
    ASSERT_EQ(routed.kind, EditorEditOutcome::Kind::RenderRouted);
    ASSERT_EQ(routed.render_command.adjustment.patches.size(), 1u);
    EXPECT_EQ(routed.render_command.adjustment.patches.front().field_key, "exposure");
  }
  EXPECT_EQ(history_->commit_count, 0);

  patch.settled = true;
  const auto settled = edit_->HandlePatch(patch, true, guard(), identity());
  ASSERT_EQ(settled.render_command.adjustment.patches.size(), 1u);
  ASSERT_TRUE(settled.render_command.adjustment.patches.front().write.has_value());
  EXPECT_EQ(test::ScalarValue(*settled.render_command.adjustment.patches.front().write), 49.0f);
  EXPECT_EQ(history_->commit_count, 1);
}

TEST_F(EditorSessionEditControllerTest, UndoAdvancesWithEmptyRenderAdjustment) {
  auto result = edit_->HandleUndoRedo(true, guard(), identity());
  EXPECT_EQ(result.kind, EditorEditOutcome::Kind::Accepted);
  EXPECT_EQ(history_->undo_count, 1);
  EXPECT_TRUE(result.render_command.adjustment.patches.empty());
  EXPECT_TRUE(result.render_command.adjustment.params_json.empty());
}

TEST_F(EditorSessionEditControllerTest, UndoFailureReturnsFailed) {
  history_->fail_undo = true;
  auto result         = edit_->HandleUndoRedo(true, guard(), identity());
  EXPECT_EQ(result.kind, EditorEditOutcome::Kind::Failed);
  EXPECT_EQ(result.message, "undo failed");
}

TEST_F(EditorSessionEditControllerTest, DiscardUsesJournalPortAndLeavesRenderAdjustmentEmpty) {
  auto result = edit_->HandleDiscard(guard(), identity(), EditorSessionState::Interactive);
  EXPECT_EQ(result.kind, EditorEditOutcome::Kind::Accepted);
  EXPECT_EQ(journal_->discard_count, 1);
  EXPECT_TRUE(result.render_command.adjustment.patches.empty());
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
  EXPECT_TRUE(result.render_command.live_parameters_applied);
  ASSERT_EQ(result.render_command.adjustment.patches.size(), 2u);
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
  EXPECT_TRUE(result.render_command.live_parameters_applied);
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
  EXPECT_TRUE(restored.render_command.live_parameters_applied);
  EXPECT_EQ(history_->restore_preview_count, 2);
}

}  // namespace
}  // namespace alcedo
