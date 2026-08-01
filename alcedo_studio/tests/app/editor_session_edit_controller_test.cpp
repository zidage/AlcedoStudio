//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_session_edit_controller.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "app/editor_session_lifecycle.hpp"
#include "edit/history/edit_transaction.hpp"
#include "edit/operators/op_base.hpp"
#include "json.hpp"
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
    lifecycle_->MarkFirstFramePresented();

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
  EditorAdjustmentPatch patch;
  patch.field_key   = "exposure";
  patch.params_json = R"({"exposure":1.0})";

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
  EditorAdjustmentPatch patch;
  patch.field_key   = "exposure";
  patch.params_json = R"({"exposure":1.25})";
  const auto result = edit_->HandlePatch(patch, false, guard(), identity());
  ASSERT_EQ(result.kind, EditorEditOutcome::Kind::RenderRouted);
  ASSERT_EQ(result.render_command.adjustment.patches.size(), 1u);
  EXPECT_EQ(result.render_command.adjustment.patches.front().field_key, "exposure");
  EXPECT_EQ(result.render_command.adjustment.patches.front().params_json, R"({"exposure":1.25})");
  EXPECT_EQ(result.render_command.adjustment.fingerprint, "exposure");
}

TEST_F(EditorSessionEditControllerTest, SettledCommitFailureReturnsRejected) {
  history_->fail_commit = true;
  EditorAdjustmentPatch patch{"exposure", R"({"exposure":0.5})", true};
  auto                  result = edit_->HandlePatch(patch, true, guard(), identity());
  EXPECT_EQ(result.kind, EditorEditOutcome::Kind::Rejected);
  EXPECT_EQ(result.message, "mini-Git journal append failed");
  EXPECT_EQ(history_->commit_count, 1);
}

TEST_F(EditorSessionEditControllerTest, RepeatedInteractivePatchesOnlyStampLatestFieldOnRender) {
  EditorAdjustmentPatch patch;
  patch.field_key = "exposure";
  for (int value = 0; value < 50; ++value) {
    patch.params_json = std::string{"{\"exposure\":"} + std::to_string(value) + "}";
    const auto routed = edit_->HandlePatch(patch, false, guard(), identity());
    ASSERT_EQ(routed.kind, EditorEditOutcome::Kind::RenderRouted);
    ASSERT_EQ(routed.render_command.adjustment.patches.size(), 1u);
    EXPECT_EQ(routed.render_command.adjustment.patches.front().field_key, "exposure");
  }
  EXPECT_EQ(history_->commit_count, 0);

  patch.settled = true;
  const auto settled = edit_->HandlePatch(patch, true, guard(), identity());
  ASSERT_EQ(settled.render_command.adjustment.patches.size(), 1u);
  EXPECT_EQ(settled.render_command.adjustment.patches.front().params_json, R"({"exposure":49})");
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
  EditorAdjustmentPatch patch{"", R"({})", false};
  auto                  result = edit_->HandlePatch(patch, false, guard(), identity());
  EXPECT_EQ(result.kind, EditorEditOutcome::Kind::Rejected);
}

TEST_F(EditorSessionEditControllerTest, PatchWithoutValidGuardIsRejected) {
  EditorHistoryGuardHandle invalid_guard{};
  EditorAdjustmentPatch    patch{"exposure", R"({"exposure":1.0})", false};
  auto                     result = edit_->HandlePatch(patch, false, invalid_guard, identity());
  EXPECT_EQ(result.kind, EditorEditOutcome::Kind::Rejected);
}

}  // namespace
}  // namespace alcedo
