//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_session_navigation_controller.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "app/editor_save_checkpoint_service.hpp"
#include "app/editor_session_lifecycle.hpp"
#include "app/editor_session_ports.hpp"

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
  auto Acquire(sl_element_id_t element_id, std::string*) -> EditorHistoryGuardHandle override {
    return {element_id, true};
  }
  void Release(const EditorHistoryGuardHandle&) override {}
  auto Undo(const EditorHistoryGuardHandle&, std::string*) -> bool override { return true; }
  auto Redo(const EditorHistoryGuardHandle&, std::string*) -> bool override { return true; }
  auto ReadAdjustmentSnapshot(const EditorHistoryGuardHandle&, EditorRenderAdjustmentSnapshot*,
                              std::string*) -> bool override {
    return true;
  }
};

class FakeTaskPort final : public IEditorTaskPort {
 public:
  auto BeginTask(const std::string&, sl_element_id_t) -> std::uint64_t override {
    return ++next_id;
  }
  void          EndTask(std::uint64_t, bool, const std::string&) override {}
  std::uint64_t next_id = 0;
};

class FakeJournalPort final : public IEditorJournalPort {
 public:
  bool                        async_commit      = false;
  bool                        async_materialize = true;
  EditorJournalCommitCallback pending_commit;
  EditorMaterializeCallback   pending_materialize;

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
    if (!async_materialize) {
      return IEditorJournalPort::MaterializeAsync(element_id, session_generation,
                                                  std::move(callback));
    }
    pending_materialize = std::move(callback);
    return true;
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
    pipeline_ = std::make_shared<FakePipelinePort>();
    history_  = std::make_shared<FakeHistoryPort>();
    tasks_    = std::make_shared<FakeTaskPort>();
    journal_  = std::make_shared<FakeJournalPort>();

    EditorSessionLifecycle::Dependencies life_deps;
    life_deps.pipeline = pipeline_;
    life_deps.history  = history_;
    lifecycle_         = std::make_unique<EditorSessionLifecycle>(std::move(life_deps));

    EditorSaveCheckpointService::Dependencies save_deps;
    save_deps.journal = journal_;
    save_deps.tasks   = tasks_;
    save_service_     = std::make_unique<EditorSaveCheckpointService>(std::move(save_deps));

    EditorSessionNavigationController::Dependencies nav_deps{
        *lifecycle_,
        *save_service_,
        [this](bool persist, bool start_save, std::string* error) {
          seal_called_     = true;
          seal_persist_    = persist;
          seal_start_save_ = start_save;
          if (seal_fail_ && error) {
            *error = "seal failed";
            return false;
          }
          if (start_save) {
            SaveCheckpointRequest req;
            req.element_id         = lifecycle_->identity().element_id;
            req.session_generation = lifecycle_->identity().session_generation;
            return save_service_->Start(
                req, [](std::uint64_t) {}, [this](const SaveCheckpointResult&) {});
          }
          return true;
        },
        [this] { release_guards_called_ = true; },
        [this] { reset_state_called_ = true; },
        [this](sl_element_id_t element_id, image_id_t image_id, bool is_switch) {
          continue_called_     = true;
          continue_element_id_ = element_id;
          continue_image_id_   = image_id;
          continue_is_switch_  = is_switch;
          lifecycle_->AdvanceSessionGeneration(element_id, image_id);
          lifecycle_->TransitionTo(EditorSessionState::Loading,
                                   EditorSessionResultKind::StateChanged, "Loading");
          EditorSessionResult r;
          r.kind     = EditorSessionResultKind::StateChanged;
          r.state    = lifecycle_->state();
          r.identity = lifecycle_->identity();
          return r;
        },
        [this](EditorSessionResult result) { emitted_results_.push_back(result); },
    };

    nav_ = std::make_unique<EditorSessionNavigationController>(std::move(nav_deps));
  }

  void OpenA() {
    lifecycle_->AdvanceSessionGeneration(1, 2);
    lifecycle_->TransitionTo(EditorSessionState::Interactive, EditorSessionResultKind::ImageReady,
                             "ready");
  }

  std::shared_ptr<FakePipelinePort>                  pipeline_;
  std::shared_ptr<FakeHistoryPort>                   history_;
  std::shared_ptr<FakeTaskPort>                      tasks_;
  std::shared_ptr<FakeJournalPort>                   journal_;
  std::unique_ptr<EditorSessionLifecycle>            lifecycle_;
  std::unique_ptr<EditorSaveCheckpointService>       save_service_;
  std::unique_ptr<EditorSessionNavigationController> nav_;

  bool                                               seal_called_           = false;
  bool                                               seal_fail_             = false;
  bool                                               seal_persist_          = false;
  bool                                               seal_start_save_       = false;
  bool                                               release_guards_called_ = false;
  bool                                               reset_state_called_    = false;
  bool                                               continue_called_       = false;
  sl_element_id_t                                    continue_element_id_   = 0;
  image_id_t                                         continue_image_id_     = 0;
  bool                                               continue_is_switch_    = false;
  std::vector<EditorSessionResult>                   emitted_results_;
};

TEST_F(EditorSessionNavigationControllerTest, OpenWithNoPriorImageContinuesImmediately) {
  const auto result = nav_->RequestOpenOrSwitch(10, 20, false);
  EXPECT_TRUE(result.completed_synchronously);
  EXPECT_FALSE(result.waiting_for_checkpoint);
  EXPECT_TRUE(continue_called_);
  EXPECT_EQ(continue_element_id_, static_cast<sl_element_id_t>(10));
  EXPECT_EQ(continue_image_id_, static_cast<image_id_t>(20));
}

TEST_F(EditorSessionNavigationControllerTest, SwitchToBWaitsForASaveCheckpoint) {
  journal_->async_commit = true;
  OpenA();

  const auto result = nav_->RequestOpenOrSwitch(3, 4, true);
  EXPECT_TRUE(result.waiting_for_checkpoint);
  EXPECT_FALSE(result.completed_synchronously);
  EXPECT_TRUE(seal_called_);
  EXPECT_TRUE(seal_start_save_);
  EXPECT_TRUE(nav_->has_pending_action());
  EXPECT_FALSE(continue_called_);

  journal_->CompleteCommit(true);
  journal_->CompleteMaterialization(true);
  nav_->ResumeAfterSave(true, {});
  EXPECT_TRUE(continue_called_);
  EXPECT_EQ(continue_element_id_, static_cast<sl_element_id_t>(3));
  EXPECT_TRUE(release_guards_called_);
  EXPECT_FALSE(nav_->has_pending_action());
}

TEST_F(EditorSessionNavigationControllerTest, SaveFailureKeepsAAndDoesNotContinueToB) {
  journal_->async_commit = true;
  OpenA();

  const auto result = nav_->RequestOpenOrSwitch(3, 4, true);
  EXPECT_TRUE(result.waiting_for_checkpoint);

  journal_->CompleteCommit(true);
  journal_->CompleteMaterialization(false, "A materialization failed");
  nav_->ResumeAfterSave(false, "A materialization failed");

  EXPECT_FALSE(continue_called_);
  EXPECT_FALSE(release_guards_called_);
  EXPECT_FALSE(nav_->has_pending_action());
  ASSERT_FALSE(emitted_results_.empty());
  EXPECT_EQ(emitted_results_.back().kind, EditorSessionResultKind::Failed);
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
  EXPECT_TRUE(reset_state_called_);
}

TEST_F(EditorSessionNavigationControllerTest, CloseWaitsForSaveThenResetsState) {
  journal_->async_commit = true;
  OpenA();

  const auto result = nav_->RequestClose(true);
  EXPECT_TRUE(result.waiting_for_checkpoint);
  EXPECT_TRUE(nav_->has_pending_action());

  journal_->CompleteCommit(true);
  journal_->CompleteMaterialization(true);
  nav_->ResumeAfterSave(true, {});
  EXPECT_TRUE(release_guards_called_);
  EXPECT_TRUE(reset_state_called_);
  EXPECT_FALSE(nav_->has_pending_action());
  ASSERT_FALSE(emitted_results_.empty());
  EXPECT_EQ(emitted_results_.back().state, EditorSessionState::NoImage);
}

TEST_F(EditorSessionNavigationControllerTest, SealFailureReportsFailureAndNoPendingAction) {
  seal_fail_ = true;
  OpenA();

  const auto result = nav_->RequestOpenOrSwitch(3, 4, true);
  EXPECT_TRUE(result.failed);
  EXPECT_FALSE(nav_->has_pending_action());
  EXPECT_FALSE(continue_called_);
}

}  // namespace
}  // namespace alcedo