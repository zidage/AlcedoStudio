//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_session_edit_controller.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "app/editor_session_lifecycle.hpp"
#include "app/editor_session_ports.hpp"
#include "edit/history/edit_transaction.hpp"
#include "edit/operators/op_base.hpp"
#include "json.hpp"

namespace alcedo {
namespace {

class FakePipelinePort final : public IEditorPipelinePort {
 public:
  auto Acquire(sl_element_id_t element_id, std::string*) -> EditorPipelineGuardHandle override {
    return {element_id, true};
  }
  void Release(const EditorPipelineGuardHandle&) override {}
};

class FakeHistoryPort final : public IEditorHistoryPort {
 public:
  bool                           fail_commit   = false;
  bool                           fail_undo     = false;
  bool                           fail_snapshot = false;
  int                            capture_count = 0;
  int                            commit_count  = 0;
  int                            undo_count    = 0;
  int                            redo_count    = 0;
  EditorRenderAdjustmentSnapshot current_snapshot{};

  auto Acquire(sl_element_id_t element_id, std::string*) -> EditorHistoryGuardHandle override {
    return {element_id, true};
  }
  void Release(const EditorHistoryGuardHandle&) override {}
  auto CaptureAdjustmentBeforePreview(const EditorHistoryGuardHandle&,
                                      const EditorAdjustmentPatch& patch, std::string*)
      -> bool override {
    ++capture_count;
    last_captured_patch = patch;
    return true;
  }
  auto CommitAdjustment(const EditorHistoryGuardHandle&, const EditorAdjustmentPatch& patch,
                        std::string* error) -> bool override {
    ++commit_count;
    last_committed_patch = patch;
    if (fail_commit) {
      if (error) {
        *error = "mini-Git journal append failed";
      }
      return false;
    }
    return true;
  }
  auto Undo(const EditorHistoryGuardHandle&, std::string* error) -> bool override {
    ++undo_count;
    if (fail_undo) {
      if (error) {
        *error = "undo failed";
      }
      return false;
    }
    return true;
  }
  auto Redo(const EditorHistoryGuardHandle&, std::string*) -> bool override {
    ++redo_count;
    return true;
  }
  auto ReadAdjustmentSnapshot(const EditorHistoryGuardHandle&,
                              EditorRenderAdjustmentSnapshot* snapshot, std::string* error)
      -> bool override {
    if (fail_snapshot) {
      if (error) {
        *error = "snapshot read failed";
      }
      return false;
    }
    if (snapshot) {
      *snapshot = current_snapshot;
    }
    return true;
  }
  EditorAdjustmentPatch last_captured_patch{};
  EditorAdjustmentPatch last_committed_patch{};
};

class FakeJournalPort final : public IEditorJournalPort {
 public:
  int           discard_count       = 0;
  int           edit_record_count   = 0;
  int           cursor_record_count = 0;
  std::uint64_t last_cursor_from    = 0;
  std::uint64_t last_cursor_to      = 0;

  auto          DiscardUnflushed(sl_element_id_t, std::string*) -> bool override {
    ++discard_count;
    return true;
  }
  auto RecordEdit(sl_element_id_t, std::uint64_t, const EditTransaction&, std::string*)
      -> bool override {
    ++edit_record_count;
    return true;
  }
  auto RecordCursorMove(sl_element_id_t, std::uint64_t, std::uint64_t from_cursor,
                        std::uint64_t to_cursor, std::string*) -> bool override {
    ++cursor_record_count;
    last_cursor_from = from_cursor;
    last_cursor_to   = to_cursor;
    return true;
  }
};

class EditorSessionEditControllerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    pipeline_ = std::make_shared<FakePipelinePort>();
    history_  = std::make_shared<FakeHistoryPort>();
    journal_  = std::make_shared<FakeJournalPort>();

    EditorSessionLifecycle::Dependencies life_deps;
    life_deps.pipeline = pipeline_;
    life_deps.history  = history_;
    lifecycle_         = std::make_unique<EditorSessionLifecycle>(std::move(life_deps));

    // Set up an interactive image.
    lifecycle_->AdvanceSessionGeneration(1, 2);
    lifecycle_->AcquireGuards(1, nullptr);
    lifecycle_->TransitionTo(EditorSessionState::Interactive, EditorSessionResultKind::ImageReady,
                             "ready");

    render_request_count_ = 0;
    last_render_reason_   = EditorRenderReason::ZoomPan;

    EditorSessionEditController::Dependencies edit_deps{
        history_,
        journal_,
        *lifecycle_,
        [this](EditorRenderReason reason, EditorRenderSupersessionPolicy) {
          ++render_request_count_;
          last_render_reason_ = reason;
          return render_request_id_;
        },
    };
    edit_ = std::make_unique<EditorSessionEditController>(std::move(edit_deps));
  }

  std::shared_ptr<FakePipelinePort>            pipeline_;
  std::shared_ptr<FakeHistoryPort>             history_;
  std::shared_ptr<FakeJournalPort>             journal_;
  std::unique_ptr<EditorSessionLifecycle>      lifecycle_;
  std::unique_ptr<EditorSessionEditController> edit_;

  int                                          render_request_count_ = 0;
  EditorRenderReason                           last_render_reason_   = EditorRenderReason::ZoomPan;
  std::uint64_t                                render_request_id_    = 1;
};

TEST_F(EditorSessionEditControllerTest, InteractiveAndSettledPatchUseOneHistoryCommit) {
  EditorAdjustmentPatch patch;
  patch.field_key   = "exposure";
  patch.params_json = R"({"exposure":1.0})";

  auto r1           = edit_->HandlePatch(patch, false);
  EXPECT_EQ(r1.kind, EditorEditResultKind::RenderRouted);
  EXPECT_EQ(last_render_reason_, EditorRenderReason::InteractiveAdjustment);
  EXPECT_EQ(history_->capture_count, 1);
  EXPECT_EQ(history_->commit_count, 0);

  patch.settled = true;
  auto r2       = edit_->HandlePatch(patch, true);
  EXPECT_EQ(r2.kind, EditorEditResultKind::RenderRouted);
  EXPECT_EQ(last_render_reason_, EditorRenderReason::SettledAdjustment);
  EXPECT_EQ(history_->capture_count, 2);
  EXPECT_EQ(history_->commit_count, 1);
  EXPECT_EQ(history_->last_committed_patch.field_key, "exposure");
  EXPECT_TRUE(history_->last_committed_patch.settled);
}

TEST_F(EditorSessionEditControllerTest, SettledCommitFailureReturnsRejected) {
  history_->fail_commit = true;
  EditorAdjustmentPatch patch{"exposure", R"({"exposure":0.5})", true};
  auto                  result = edit_->HandlePatch(patch, true);
  EXPECT_EQ(result.kind, EditorEditResultKind::Rejected);
  EXPECT_EQ(result.message, "mini-Git journal append failed");
  EXPECT_EQ(history_->commit_count, 1);
  EXPECT_EQ(render_request_count_, 0);
}

TEST_F(EditorSessionEditControllerTest, RepeatedInteractivePatchesKeepLatestValuePerField) {
  EditorAdjustmentPatch patch;
  patch.field_key = "exposure";
  for (int value = 0; value < 50; ++value) {
    patch.params_json = std::string{"{\"exposure\":"} + std::to_string(value) + "}";
    edit_->HandlePatch(patch, false);
  }
  const auto snapshot = edit_->adjustment_snapshot();
  ASSERT_EQ(snapshot.patches.size(), 1u);
  EXPECT_EQ(snapshot.patches.front().field_key, "exposure");
  EXPECT_EQ(snapshot.patches.front().params_json, R"({"exposure":49})");
  EXPECT_EQ(history_->commit_count, 0);

  patch.settled = true;
  edit_->HandlePatch(patch, true);
  EXPECT_EQ(history_->commit_count, 1);
}

TEST_F(EditorSessionEditControllerTest, UndoAdvancesRenderGenerationAndRoutes) {
  const auto render_before = lifecycle_->identity().render_generation;
  auto       result        = edit_->HandleUndoRedo(true);
  EXPECT_EQ(result.kind, EditorEditResultKind::Accepted);
  EXPECT_EQ(history_->undo_count, 1);
  EXPECT_GT(lifecycle_->identity().render_generation, render_before);
  EXPECT_EQ(last_render_reason_, EditorRenderReason::UndoRedo);
}

TEST_F(EditorSessionEditControllerTest, UndoFailureReturnsFailed) {
  history_->fail_undo = true;
  auto result         = edit_->HandleUndoRedo(true);
  EXPECT_EQ(result.kind, EditorEditResultKind::Failed);
  EXPECT_EQ(result.message, "undo failed");
}

TEST_F(EditorSessionEditControllerTest, DiscardUsesJournalPortAndRestoresSnapshot) {
  history_->current_snapshot.params_json = R"({"contrast":0.0})";
  auto result                            = edit_->HandleDiscard();
  EXPECT_EQ(result.kind, EditorEditResultKind::Accepted);
  EXPECT_EQ(journal_->discard_count, 1);
  EXPECT_EQ(edit_->adjustment_snapshot().params_json, R"({"contrast":0.0})");
}

TEST_F(EditorSessionEditControllerTest, RecordFinalizedEditReachesJournalPort) {
  EditTransaction txn{TransactionType::_EDIT, OperatorType::EXPOSURE,
                      PipelineStageName::Basic_Adjustment, nlohmann::json{{"exposure", 1.0}}};
  std::string     error;
  EXPECT_TRUE(edit_->RecordFinalizedEdit(txn, &error)) << error;
  EXPECT_EQ(journal_->edit_record_count, 1);
}

TEST_F(EditorSessionEditControllerTest, RecordHistoryCursorMoveReachesJournalPort) {
  std::string error;
  EXPECT_TRUE(edit_->RecordHistoryCursorMove(2, 1, &error)) << error;
  EXPECT_EQ(journal_->cursor_record_count, 1);
  EXPECT_EQ(journal_->last_cursor_from, 2u);
  EXPECT_EQ(journal_->last_cursor_to, 1u);
}

TEST_F(EditorSessionEditControllerTest, PatchWhileNotInteractiveIsRejected) {
  lifecycle_->TransitionTo(EditorSessionState::Loading, EditorSessionResultKind::StateChanged,
                           "Loading");
  EditorAdjustmentPatch patch{"exposure", R"({"exposure":1.0})", false};
  auto                  result = edit_->HandlePatch(patch, false);
  EXPECT_EQ(result.kind, EditorEditResultKind::Rejected);
  EXPECT_EQ(render_request_count_, 0);
}

}  // namespace
}  // namespace alcedo