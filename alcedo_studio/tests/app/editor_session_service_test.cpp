//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_session_service.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "app/editor_render_coordinator.hpp"
#include "app/editor_session_bootstrap.hpp"

namespace alcedo {
namespace {

class FakePipelinePort final : public IEditorPipelinePort {
 public:
  bool fail_acquire = false;
  int  acquire_count = 0;
  int  release_count = 0;

  auto Acquire(sl_element_id_t element_id, std::string* error) -> EditorPipelineGuardHandle override {
    ++acquire_count;
    if (fail_acquire) {
      if (error) {
        *error = "pipeline acquire failed";
      }
      return {};
    }
    return EditorPipelineGuardHandle{element_id, true};
  }
  void Release(const EditorPipelineGuardHandle&) override { ++release_count; }
};

class FakeHistoryPort final : public IEditorHistoryPort {
 public:
  bool fail_acquire = false;
  bool fail_undo    = false;
  int  undo_count   = 0;
  int  redo_count   = 0;

  auto Acquire(sl_element_id_t element_id, std::string* error) -> EditorHistoryGuardHandle override {
    if (fail_acquire) {
      if (error) {
        *error = "history acquire failed";
      }
      return {};
    }
    return EditorHistoryGuardHandle{element_id, true};
  }
  void Release(const EditorHistoryGuardHandle&) override {}
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
};

class FakeTaskPort final : public IEditorTaskPort {
 public:
  int begin_count = 0;
  int end_count   = 0;
  auto BeginTask(const std::string&, sl_element_id_t) -> std::uint64_t override {
    ++begin_count;
    return 1;
  }
  void EndTask(std::uint64_t, bool, const std::string&) override { ++end_count; }
};

class FakeJournalPort final : public IEditorJournalPort {
 public:
  int barrier_count = 0;
  int discard_count = 0;
  auto AppendBarrier(sl_element_id_t, std::uint64_t, std::string*) -> bool override {
    ++barrier_count;
    return true;
  }
  auto DiscardUnflushed(sl_element_id_t, std::string*) -> bool override {
    ++discard_count;
    return true;
  }
};

class RecordingRenderPort final : public IEditorRenderSubmitPort {
 public:
  std::vector<EditorRenderIntent> submitted;
  std::vector<std::uint64_t>      cancelled_sessions;
  std::uint64_t                   next_id = 1;
  std::uint64_t                   active_session = 0;
  std::uint64_t                   active_render  = 0;
  std::uint64_t                   active_view    = 0;

  auto Submit(const EditorRenderIntent& intent) -> EditorRenderResult override {
    submitted.push_back(intent);
    EditorRenderResult result;
    result.kind       = EditorRenderResultKind::RequestAccepted;
    result.request_id = next_id++;
    result.intent     = intent;
    return result;
  }
  void CancelSession(std::uint64_t session_generation) override {
    cancelled_sessions.push_back(session_generation);
  }
  void SetActiveGenerations(std::uint64_t session_generation, std::uint64_t render_generation,
                            std::uint64_t view_generation) override {
    active_session = session_generation;
    active_render  = render_generation;
    active_view    = view_generation;
  }
};

class EditorSessionServiceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    pipeline_ = std::make_shared<FakePipelinePort>();
    history_  = std::make_shared<FakeHistoryPort>();
    tasks_    = std::make_shared<FakeTaskPort>();
    journal_  = std::make_shared<FakeJournalPort>();
    render_   = std::make_shared<RecordingRenderPort>();

    EditorSessionService::Dependencies deps;
    deps.pipeline = pipeline_;
    deps.history  = history_;
    deps.tasks    = tasks_;
    deps.journal  = journal_;
    deps.render   = render_;
    service_      = std::make_unique<EditorSessionService>(std::move(deps));
  }

  std::shared_ptr<FakePipelinePort>      pipeline_;
  std::shared_ptr<FakeHistoryPort>       history_;
  std::shared_ptr<FakeTaskPort>          tasks_;
  std::shared_ptr<FakeJournalPort>       journal_;
  std::shared_ptr<RecordingRenderPort>   render_;
  std::unique_ptr<EditorSessionService>  service_;
};

TEST_F(EditorSessionServiceTest, OpenAcquiresGuardsRoutesInitialRenderAndLeavesLoading) {
  const auto result = service_->Open(100, 200);
  EXPECT_EQ(service_->state(), EditorSessionState::Loading);
  EXPECT_TRUE(service_->has_image());
  EXPECT_EQ(service_->identity().element_id, 100u);
  EXPECT_EQ(service_->identity().image_id, 200u);
  EXPECT_EQ(pipeline_->acquire_count, 1);
  ASSERT_EQ(render_->submitted.size(), 1u);
  EXPECT_EQ(render_->submitted.front().reason, EditorRenderReason::InitialFrame);
  EXPECT_EQ(render_->submitted.front().quality, EditorRenderQuality::Interactive);
  EXPECT_EQ(result.kind, EditorSessionResultKind::RenderRouted);
}

TEST_F(EditorSessionServiceTest, StateMachineIgnoresReorderedStaleCompletions) {
  service_->Open(1, 2);
  const auto gen_a = service_->identity().session_generation;
  service_->NotifyImageAcquired(gen_a, true);
  EXPECT_EQ(service_->state(), EditorSessionState::Interactive);

  service_->Switch(3, 4);
  const auto gen_b = service_->identity().session_generation;
  EXPECT_NE(gen_a, gen_b);
  EXPECT_EQ(service_->state(), EditorSessionState::Loading);

  // Late save completion for A and late failure for A must not clobber B.
  service_->NotifySaveFinished(gen_a, false, "stale save failure");
  service_->NotifyImageAcquired(gen_a, false, "stale load failure");
  EXPECT_EQ(service_->state(), EditorSessionState::Loading);
  EXPECT_EQ(service_->identity().element_id, 3u);

  service_->NotifyImageAcquired(gen_b, true);
  EXPECT_EQ(service_->state(), EditorSessionState::Interactive);
}

TEST_F(EditorSessionServiceTest, SwitchCancelsPriorSessionAndSealsJournal) {
  service_->Open(1, 2);
  const auto gen_a = service_->identity().session_generation;
  service_->NotifyImageAcquired(gen_a, true);
  service_->Switch(9, 10);
  ASSERT_FALSE(render_->cancelled_sessions.empty());
  EXPECT_EQ(render_->cancelled_sessions.front(), gen_a);
  EXPECT_GE(journal_->barrier_count, 1);
  EXPECT_GE(tasks_->begin_count, 1);
  ASSERT_FALSE(render_->submitted.empty());
  EXPECT_EQ(render_->submitted.back().reason, EditorRenderReason::ImageSwitch);
}

TEST_F(EditorSessionServiceTest, PatchAndGestureCommitRouteThroughRenderPortOnly) {
  service_->Open(1, 2);
  service_->NotifyImageAcquired(service_->identity().session_generation, true);
  render_->submitted.clear();

  service_->Patch("exposure");
  ASSERT_EQ(render_->submitted.size(), 1u);
  EXPECT_EQ(render_->submitted.back().reason, EditorRenderReason::InteractiveAdjustment);

  service_->GestureCommit("exposure");
  ASSERT_EQ(render_->submitted.size(), 2u);
  EXPECT_EQ(render_->submitted.back().reason, EditorRenderReason::SettledAdjustment);
}

TEST_F(EditorSessionServiceTest, UndoRedoAdvanceRenderGenerationAndRoute) {
  service_->Open(1, 2);
  service_->NotifyImageAcquired(service_->identity().session_generation, true);
  const auto render_before = service_->identity().render_generation;
  render_->submitted.clear();
  service_->Undo();
  EXPECT_EQ(history_->undo_count, 1);
  EXPECT_GT(service_->identity().render_generation, render_before);
  ASSERT_EQ(render_->submitted.size(), 1u);
  EXPECT_EQ(render_->submitted.back().reason, EditorRenderReason::UndoRedo);
}

TEST_F(EditorSessionServiceTest, DiscardUsesJournalPort) {
  service_->Open(1, 2);
  service_->NotifyImageAcquired(service_->identity().session_generation, true);
  service_->Discard();
  EXPECT_EQ(journal_->discard_count, 1);
}

TEST_F(EditorSessionServiceTest, ShutdownReleasesGuardsAndRejectsFurtherOpens) {
  service_->Open(1, 2);
  service_->Shutdown();
  EXPECT_EQ(service_->state(), EditorSessionState::ShuttingDown);
  EXPECT_EQ(pipeline_->release_count, 1);
  const auto rejected = service_->Open(5, 6);
  EXPECT_EQ(rejected.kind, EditorSessionResultKind::Rejected);
}

TEST_F(EditorSessionServiceTest, AcquireFailureEntersFailedState) {
  pipeline_->fail_acquire = true;
  const auto result = service_->Open(1, 2);
  EXPECT_EQ(service_->state(), EditorSessionState::Failed);
  EXPECT_EQ(result.kind, EditorSessionResultKind::Failed);
  EXPECT_TRUE(render_->submitted.empty());
}

TEST_F(EditorSessionServiceTest, FramePresentedMovesLoadingToInteractive) {
  service_->Open(1, 2);
  EXPECT_EQ(service_->state(), EditorSessionState::Loading);
  EditorRenderResult presented;
  presented.kind = EditorRenderResultKind::FramePresented;
  presented.intent.session_generation = service_->identity().session_generation;
  service_->NotifyRenderResult(presented);
  EXPECT_EQ(service_->state(), EditorSessionState::Interactive);
}

TEST_F(EditorSessionServiceTest, WorksWithRealCoordinatorAsRenderPort) {
  auto scheduler = std::make_shared<EditorSessionBootstrapSchedulerPort>();
  auto coordinator = std::make_shared<EditorRenderCoordinator>(scheduler);
  EditorSessionService::Dependencies deps;
  deps.pipeline = pipeline_;
  deps.history  = history_;
  deps.tasks    = tasks_;
  deps.journal  = journal_;
  deps.render   = coordinator;
  EditorSessionService service(std::move(deps));
  service.Open(7, 8);
  EXPECT_EQ(service.state(), EditorSessionState::Loading);
  EXPECT_EQ(scheduler->scheduled().size(), 1u);
  EXPECT_EQ(scheduler->scheduled().front().intent.reason, EditorRenderReason::InitialFrame);
}

}  // namespace
}  // namespace alcedo
