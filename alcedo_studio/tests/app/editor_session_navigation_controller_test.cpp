//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_session_navigation_controller.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "app/editor_save_checkpoint_service.hpp"
#include "app/editor_session_edit_controller.hpp"
#include "app/editor_session_lifecycle.hpp"
#include "app/editor_session_ports.hpp"
#include "app/editor_session_render_controller.hpp"

namespace alcedo {
namespace {

class FakePipelinePort final : public IEditorPipelinePort {
 public:
  auto Acquire(sl_element_id_t element_id, std::string*) -> EditorPipelineGuardHandle override {
    ++acquire_count;
    return {element_id, true};
  }
  void Release(const EditorPipelineGuardHandle&) override {}
  int  acquire_count = 0;
};

class FakeHistoryPort final : public IEditorHistoryPort {
 public:
  auto Acquire(sl_element_id_t element_id, std::string*) -> EditorHistoryGuardHandle override {
    ++acquire_count;
    return {element_id, true};
  }
  void Release(const EditorHistoryGuardHandle&) override {}
  auto Undo(const EditorHistoryGuardHandle&, std::string*) -> bool override { return true; }
  auto Redo(const EditorHistoryGuardHandle&, std::string*) -> bool override { return true; }
  auto ReadAdjustmentSnapshot(const EditorHistoryGuardHandle&, EditorRenderAdjustmentSnapshot*,
                              std::string*) -> bool override {
    return true;
  }
  int acquire_count = 0;
};

class FakeTaskPort final : public IEditorTaskPort {
 public:
  auto BeginTask(const std::string&, sl_element_id_t) -> std::uint64_t override {
    return ++next_id;
  }
  void          EndTask(std::uint64_t, bool, const std::string&) override {}
  std::uint64_t next_id = 0;
};

class FakeRenderPort final : public IEditorRenderSubmitPort {
 public:
  void CancelSessionAndWait(std::uint64_t) override {}
  void CancelSession(std::uint64_t) override {}
  auto Submit(const EditorRenderIntent&) -> EditorRenderResult override {
    EditorRenderResult r;
    r.kind       = EditorRenderResultKind::RequestAccepted;
    r.request_id = 1;
    return r;
  }
  void SetActiveGenerations(std::uint64_t, std::uint64_t, std::uint64_t,
                            EditorRenderSupersessionPolicy) override {}
};

class FakeJournalPort final : public IEditorJournalPort {
 public:
  bool                        async_commit = false;
  bool                        fail_barrier = false;
  EditorJournalCommitCallback pending_commit;
  EditorMaterializeCallback   pending_materialize;
  bool                        finalize_succeeds = true;

  auto FinalizeEdit(sl_element_id_t, std::uint64_t, std::string* error) -> bool override {
    if (!finalize_succeeds && error) {
      *error = "finalize failed";
    }
    return finalize_succeeds;
  }
  auto AppendBarrier(sl_element_id_t, std::uint64_t, std::string* error) -> bool override {
    if (fail_barrier) {
      if (error) {
        *error = "journal barrier failed";
      }
      return false;
    }
    return true;
  }
  auto CommitJournalAsync(sl_element_id_t element_id, std::uint64_t session_generation,
                          EditorJournalCommitCallback callback) -> bool override {
    if (!async_commit) {
      return IEditorJournalPort::CommitJournalAsync(element_id, session_generation,
                                                    std::move(callback));
    }
    pending_commit = std::move(callback);
    return true;
  }
  auto MaterializeAsync(sl_element_id_t element_id, std::uint64_t session_generation,
                        EditorMaterializeCallback callback) -> bool override {
    if (async_commit) {
      pending_materialize = std::move(callback);
      return true;
    }
    return IEditorJournalPort::MaterializeAsync(element_id, session_generation,
                                                std::move(callback));
  }
  void CompleteCommit(bool durable) {
    ASSERT_TRUE(static_cast<bool>(pending_commit));
    auto cb = std::move(pending_commit);
    cb(EditorJournalCommitOutcome{
        true, durable, !durable, durable ? 2u : 0u, durable ? 1u : 0u, {}});
  }
  void CompleteMaterialization(bool materialized, std::string error = {}) {
    ASSERT_TRUE(static_cast<bool>(pending_materialize));
    auto cb = std::move(pending_materialize);
    cb(EditorMaterializeOutcome{true, materialized, materialized ? 1u : 0u, std::move(error)});
  }
  auto DiscardUnflushed(sl_element_id_t, std::string*) -> bool override { return true; }
};

class EditorSessionNavigationControllerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    pipeline_    = std::make_shared<FakePipelinePort>();
    history_     = std::make_shared<FakeHistoryPort>();
    tasks_       = std::make_shared<FakeTaskPort>();
    journal_     = std::make_shared<FakeJournalPort>();
    render_port_ = std::make_shared<FakeRenderPort>();

    EditorSessionLifecycle::Dependencies life_deps;
    life_deps.pipeline = pipeline_;
    life_deps.history  = history_;
    lifecycle_         = std::make_unique<EditorSessionLifecycle>(std::move(life_deps));

    EditorSaveCheckpointService::Dependencies save_deps;
    save_deps.journal = journal_;
    save_deps.tasks   = tasks_;
    save_service_     = std::make_unique<EditorSaveCheckpointService>(std::move(save_deps));

    EditorSessionRenderController::Dependencies render_deps{
        render_port_,
        [](const EditorRenderEvent&) {},
    };
    render_ = std::make_unique<EditorSessionRenderController>(std::move(render_deps));

    EditorSessionEditController::Dependencies edit_deps{history_, journal_};
    edit_ = std::make_unique<EditorSessionEditController>(std::move(edit_deps));

    nav_  = std::make_unique<EditorSessionNavigationController>(
        *lifecycle_, *save_service_, *render_, *edit_, journal_.get(), history_.get());
  }

  void OpenA() {
    // Use semantic lifecycle API to simulate an open, interactive image.
    ASSERT_TRUE(lifecycle_->BeginAcquire(1, 2, false, nullptr, nullptr));
    ASSERT_TRUE(lifecycle_->AcquireGuards(nullptr));
    lifecycle_->MarkImageReady();
    lifecycle_->MarkFirstFramePresented();  // transitions to Interactive
  }

  std::shared_ptr<FakePipelinePort>                  pipeline_;
  std::shared_ptr<FakeHistoryPort>                   history_;
  std::shared_ptr<FakeTaskPort>                      tasks_;
  std::shared_ptr<FakeJournalPort>                   journal_;
  std::shared_ptr<FakeRenderPort>                    render_port_;
  std::unique_ptr<EditorSessionLifecycle>            lifecycle_;
  std::unique_ptr<EditorSaveCheckpointService>       save_service_;
  std::unique_ptr<EditorSessionRenderController>     render_;
  std::unique_ptr<EditorSessionEditController>       edit_;
  std::unique_ptr<EditorSessionNavigationController> nav_;
};

TEST_F(EditorSessionNavigationControllerTest, OpenWithNoPriorImageCompletesSynchronously) {
  const auto result = nav_->RequestOpenOrSwitch(10, 20, false);
  EXPECT_TRUE(result.completed_synchronously);
  EXPECT_FALSE(result.waiting_for_checkpoint);
  EXPECT_FALSE(result.rejected);
  EXPECT_FALSE(result.failed);
  EXPECT_TRUE(lifecycle_->has_image());
  EXPECT_EQ(lifecycle_->identity().element_id, static_cast<sl_element_id_t>(10));
  EXPECT_EQ(lifecycle_->identity().image_id, static_cast<image_id_t>(20));
}

TEST_F(EditorSessionNavigationControllerTest, SwitchToBWaitsForASaveCheckpoint) {
  journal_->async_commit = true;
  OpenA();

  const auto result = nav_->RequestOpenOrSwitch(3, 4, true);
  EXPECT_TRUE(result.waiting_for_checkpoint);
  EXPECT_FALSE(result.completed_synchronously);
  EXPECT_TRUE(nav_->has_pending_action());
  EXPECT_TRUE(result.ticket.valid());

  // Simulate successful save completion.
  journal_->CompleteCommit(true);
  journal_->CompleteMaterialization(true);
  // The navigation controller's OnCheckpointFinished is the callback registered
  // by SealAndStartSave. Since the save was async, the callback fires when
  // CompleteMaterialization finishes. Let the save service process it.
  // Note: The callback lambda captures `this` and calls OnCheckpointFinished.
  // Since CompleteMaterialization fires the callback synchronously inside
  // MaterializeAsync's completion, it should route through.
  EXPECT_FALSE(nav_->has_pending_action());
  EXPECT_TRUE(lifecycle_->has_image());
  EXPECT_EQ(lifecycle_->identity().element_id, static_cast<sl_element_id_t>(3));
}

TEST_F(EditorSessionNavigationControllerTest, SaveFailureKeepsAAndDiscardsPendingAction) {
  journal_->async_commit = true;
  OpenA();

  const auto result = nav_->RequestOpenOrSwitch(3, 4, true);
  EXPECT_TRUE(result.waiting_for_checkpoint);

  journal_->CompleteCommit(true);
  journal_->CompleteMaterialization(false, "A materialization failed");
  // OnCheckpointFinished processes the failure. The prior image A remains.
  EXPECT_FALSE(nav_->has_pending_action());
  EXPECT_EQ(lifecycle_->state(), EditorSessionState::Failed);
}

TEST_F(EditorSessionNavigationControllerTest, SecondActionDoesNotReplaceOriginalTarget) {
  journal_->async_commit = true;
  OpenA();

  const auto first = nav_->RequestOpenOrSwitch(3, 4, true);
  EXPECT_TRUE(first.waiting_for_checkpoint);
  EXPECT_TRUE(nav_->has_pending_action());

  const auto second = nav_->RequestOpenOrSwitch(5, 6, true);
  EXPECT_TRUE(second.rejected);
  EXPECT_EQ(second.message, "Editor save checkpoint is in progress");
  EXPECT_TRUE(nav_->has_pending_action());
}

TEST_F(EditorSessionNavigationControllerTest, CloseWithNoPriorImageCompletesSynchronously) {
  const auto result = nav_->RequestClose(true);
  EXPECT_TRUE(result.completed_synchronously);
  EXPECT_FALSE(result.waiting_for_checkpoint);
  EXPECT_TRUE(lifecycle_->state() == EditorSessionState::NoImage ||
              lifecycle_->state() == EditorSessionState::ShuttingDown);
}

TEST_F(EditorSessionNavigationControllerTest, CloseWaitsForSaveThenCompletes) {
  journal_->async_commit = true;
  OpenA();

  const auto result = nav_->RequestClose(true);
  EXPECT_TRUE(result.waiting_for_checkpoint);
  EXPECT_TRUE(nav_->has_pending_action());

  journal_->CompleteCommit(true);
  journal_->CompleteMaterialization(true);
  // OnCheckpointFinished should have processed the result.
  EXPECT_FALSE(nav_->has_pending_action());
}

TEST_F(EditorSessionNavigationControllerTest, ShutDownRejectsFurtherOpens) {
  OpenA();
  lifecycle_->BeginShutdown();
  const auto result = nav_->RequestOpenOrSwitch(5, 6, false);
  EXPECT_TRUE(result.rejected);
}

TEST_F(EditorSessionNavigationControllerTest, SameImageIsNoop) {
  OpenA();
  const auto identity = lifecycle_->identity();
  const auto result   = nav_->RequestOpenOrSwitch(identity.element_id, identity.image_id, false);
  EXPECT_TRUE(result.completed_synchronously);
  EXPECT_TRUE(result.same_image_noop);
}

// ── Synchronous save paths ──────────────────────────────────────────────────

TEST_F(EditorSessionNavigationControllerTest, SyncSaveSwitchCompletesImmediately) {
  // async_commit defaults to false → sync path through CommitJournalAsync.
  OpenA();
  const auto gen_a  = lifecycle_->identity().session_generation;

  const auto result = nav_->RequestOpenOrSwitch(3, 4, true);
  EXPECT_TRUE(result.completed_synchronously);
  EXPECT_FALSE(result.waiting_for_checkpoint);
  EXPECT_FALSE(nav_->has_pending_action());
  EXPECT_TRUE(lifecycle_->has_image());
  EXPECT_EQ(lifecycle_->identity().element_id, static_cast<sl_element_id_t>(3));
  EXPECT_EQ(lifecycle_->identity().image_id, static_cast<image_id_t>(4));
  EXPECT_EQ(lifecycle_->identity().session_generation, gen_a + 1);
  EXPECT_EQ(pipeline_->acquire_count, 2);
  EXPECT_EQ(history_->acquire_count, 2);
}

TEST_F(EditorSessionNavigationControllerTest, SyncSaveFailureStaysOnA) {
  journal_->fail_barrier = true;
  OpenA();
  const auto gen_a  = lifecycle_->identity().session_generation;

  const auto result = nav_->RequestOpenOrSwitch(3, 4, true);
  EXPECT_TRUE(result.failed);
  EXPECT_FALSE(result.completed_synchronously);
  EXPECT_FALSE(result.waiting_for_checkpoint);
  EXPECT_FALSE(nav_->has_pending_action());
  EXPECT_EQ(lifecycle_->identity().session_generation, gen_a);
  EXPECT_EQ(lifecycle_->state(), EditorSessionState::Failed);
}

TEST_F(EditorSessionNavigationControllerTest, SyncCloseCompletesImmediately) {
  OpenA();
  const auto result = nav_->RequestClose(true);
  EXPECT_TRUE(result.completed_synchronously);
  EXPECT_FALSE(result.waiting_for_checkpoint);
  EXPECT_FALSE(nav_->has_pending_action());
  EXPECT_TRUE(lifecycle_->state() == EditorSessionState::NoImage ||
              lifecycle_->state() == EditorSessionState::ShuttingDown);
}

// ── Stale completions ───────────────────────────────────────────────────────

TEST_F(EditorSessionNavigationControllerTest, StaleCompletionWithWrongRequestIdKeepsPendingAction) {
  journal_->async_commit = true;
  OpenA();

  const auto result = nav_->RequestOpenOrSwitch(3, 4, true);
  EXPECT_TRUE(result.waiting_for_checkpoint);
  EXPECT_TRUE(nav_->has_pending_action());

  // Fire a stale completion with a different request_id.
  SaveCheckpointResult stale;
  stale.request_id           = 999;
  stale.session_generation   = lifecycle_->identity().session_generation;
  stale.checkpoint_completed = true;
  nav_->OnCheckpointFinished(stale);

  // The pending action must NOT have been consumed.
  EXPECT_TRUE(nav_->has_pending_action());
  EXPECT_EQ(lifecycle_->state(), EditorSessionState::Saving);

  // Complete the real save.
  journal_->CompleteCommit(true);
  journal_->CompleteMaterialization(true);
  EXPECT_FALSE(nav_->has_pending_action());
  EXPECT_EQ(lifecycle_->identity().element_id, static_cast<sl_element_id_t>(3));
}

TEST_F(EditorSessionNavigationControllerTest,
       StaleCompletionWithWrongSessionGenerationKeepsPendingAction) {
  journal_->async_commit = true;
  OpenA();

  const auto result = nav_->RequestOpenOrSwitch(3, 4, true);
  EXPECT_TRUE(result.waiting_for_checkpoint);

  // Fire a stale completion with the wrong session_generation.
  // (request_id may coincidentally match but session won't).
  SaveCheckpointResult stale;
  stale.request_id           = 1;  // plausible but wrong for this save
  stale.session_generation   = 99;
  stale.checkpoint_completed = true;
  nav_->OnCheckpointFinished(stale);

  // The pending action must NOT have been consumed.
  EXPECT_TRUE(nav_->has_pending_action());
  EXPECT_EQ(lifecycle_->state(), EditorSessionState::Saving);
}

}  // namespace
}  // namespace alcedo
