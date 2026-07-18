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
  bool fail_acquire  = false;
  int  acquire_count = 0;
  int  release_count = 0;

  auto Acquire(sl_element_id_t element_id, std::string* error)
      -> EditorPipelineGuardHandle override {
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
  bool                           fail_acquire  = false;
  bool                           fail_undo     = false;
  bool                           fail_snapshot = false;
  int                            undo_count    = 0;
  int                            redo_count    = 0;
  EditorRenderAdjustmentSnapshot current_snapshot{};

  auto                           Acquire(sl_element_id_t element_id, std::string* error)
      -> EditorHistoryGuardHandle override {
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
};

class FakeTaskPort final : public IEditorTaskPort {
 public:
  bool                       fail_begin  = false;
  int                        begin_count = 0;
  int                        end_count   = 0;
  std::vector<std::uint64_t> begun_ids;
  std::vector<std::uint64_t> ended_ids;
  std::uint64_t              next_id = 1;

  auto BeginTask(const std::string&, sl_element_id_t) -> std::uint64_t override {
    ++begin_count;
    if (fail_begin) {
      return 0;
    }
    const auto id = next_id++;
    begun_ids.push_back(id);
    return id;
  }
  void EndTask(std::uint64_t task_id, bool, const std::string&) override {
    ++end_count;
    ended_ids.push_back(task_id);
  }
};

class FakeJournalPort final : public IEditorJournalPort {
 public:
  bool fail_barrier  = false;
  bool fail_discard  = false;
  int  barrier_count = 0;
  int  discard_count = 0;
  auto AppendBarrier(sl_element_id_t, std::uint64_t, std::string* error) -> bool override {
    ++barrier_count;
    if (fail_barrier) {
      if (error) {
        *error = "journal barrier failed";
      }
      return false;
    }
    return true;
  }
  auto DiscardUnflushed(sl_element_id_t, std::string* error) -> bool override {
    ++discard_count;
    if (fail_discard) {
      if (error) {
        *error = "journal discard failed";
      }
      return false;
    }
    return true;
  }
};

class RecordingRenderPort final : public IEditorRenderSubmitPort {
 public:
  bool                            fail_submit = false;
  std::vector<EditorRenderIntent> submitted;
  std::vector<std::uint64_t>      cancelled_sessions;
  std::vector<std::uint64_t>      waited_sessions;
  std::uint64_t                   next_id        = 1;
  std::uint64_t                   active_session = 0;
  std::uint64_t                   active_render  = 0;
  std::uint64_t                   active_view    = 0;

  auto Submit(const EditorRenderIntent& intent) -> EditorRenderResult override {
    submitted.push_back(intent);
    EditorRenderResult result;
    result.kind =
        fail_submit ? EditorRenderResultKind::Failed : EditorRenderResultKind::RequestAccepted;
    result.request_id = fail_submit ? 0 : next_id++;
    result.intent     = intent;
    return result;
  }
  void CancelSession(std::uint64_t session_generation) override {
    cancelled_sessions.push_back(session_generation);
  }
  void CancelSessionAndWait(std::uint64_t session_generation) override {
    CancelSession(session_generation);
    waited_sessions.push_back(session_generation);
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
    service_->SetPresentationSinkId(1);
    service_->SetPresentationSize(640, 480);
  }

  void PresentFirstFrame(EditorSessionService& service) {
    const auto gen = service.identity().session_generation;
    service.NotifyImageAcquired(gen, true);
    EditorRenderResult completed;
    completed.kind       = EditorRenderResultKind::RenderCompleted;
    completed.request_id = service.first_frame_request_id();
    completed.intent     = render_->submitted.back();
    service.NotifyRenderResult(completed);
    EditorRenderResult submitted;
    submitted.kind       = EditorRenderResultKind::FrameSubmitted;
    submitted.request_id = service.first_frame_request_id();
    submitted.intent     = render_->submitted.back();
    service.NotifyRenderResult(submitted);
    EditorRenderResult presented;
    presented.kind       = EditorRenderResultKind::FramePresented;
    presented.request_id = service.first_frame_request_id();
    presented.intent     = render_->submitted.back();
    service.NotifyRenderResult(presented);
  }

  std::shared_ptr<FakePipelinePort>     pipeline_;
  std::shared_ptr<FakeHistoryPort>      history_;
  std::shared_ptr<FakeTaskPort>         tasks_;
  std::shared_ptr<FakeJournalPort>      journal_;
  std::shared_ptr<RecordingRenderPort>  render_;
  std::unique_ptr<EditorSessionService> service_;
};

// Phase 5D: drive a real-coordinator runtime to Interactive by presenting the
// first frame through the bootstrap scheduler. Returns the first-frame request
// id so tests can correlate coordinator completions.
auto DriveRuntimeToInteractive(EditorSessionRuntime& runtime) -> std::uint64_t {
  runtime.service->SetPresentationSinkId(1);
  runtime.service->SetPresentationSize(640, 480);
  runtime.service->Open(11, 22);
  auto* bootstrap_scheduler =
      dynamic_cast<EditorSessionBootstrapSchedulerPort*>(runtime.scheduler.get());
  EXPECT_NE(bootstrap_scheduler, nullptr);
  const auto request_id = bootstrap_scheduler->scheduled().front().request_id;
  runtime.service->NotifyImageAcquired(runtime.service->identity().session_generation, true);
  runtime.coordinator->NotifySchedulerCompleted(request_id, true);
  runtime.coordinator->NotifyFrameSubmitted(request_id);
  runtime.coordinator->NotifyFramePresented(request_id);
  // Presenting the first frame transitions the session to Interactive and
  // auto-routes a QualityBase follow-up (TryEnterInteractiveFromFirstFrame ->
  // RouteQualityBaseFollowUp). Drive that follow-up to completion too so the
  // coordinator is idle and each Phase 5D view-change test starts from a clean
  // Interactive state (render_busy == false, no in-flight shadowing the next
  // intent). Without this, the QualityBase follow-up stays in-flight and a
  // subsequent DetailRefresh (view-generation advance, which does NOT cancel
  // full-frame work by design) would never get scheduled.
  if (bootstrap_scheduler->scheduled().size() > 1u) {
    const auto quality_request_id = bootstrap_scheduler->scheduled().back().request_id;
    runtime.coordinator->NotifySchedulerCompleted(quality_request_id, true);
    runtime.coordinator->NotifyFrameSubmitted(quality_request_id);
    runtime.coordinator->NotifyFramePresented(quality_request_id);
  }
  return request_id;
}

auto MakeRegion(int x) -> ViewportRenderRegion {
  ViewportRenderRegion region{};
  region.x_               = x;
  region.y_               = 0;
  region.scale_x_         = 2.0f;
  region.scale_y_         = 2.0f;
  region.reference_width_ = 640;
  region.reference_height_ = 480;
  region.target_width_    = 320;
  region.target_height_   = 240;
  return region;
}

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

TEST_F(EditorSessionServiceTest, InitialRenderWaitsForARealPresentationTarget) {
  service_->SetPresentationSinkId(0);
  const auto open = service_->Open(100, 200);

  // Phase 5B: open acquires the image immediately, then waits for a real
  // presentation target before routing the InteractivePrimary first frame.
  EXPECT_EQ(service_->state(), EditorSessionState::Loading);
  EXPECT_EQ(service_->first_frame_request_id(), 0u);
  EXPECT_TRUE(render_->submitted.empty());
  EXPECT_TRUE(open.kind == EditorSessionResultKind::StateChanged ||
              open.kind == EditorSessionResultKind::Accepted);

  service_->SetPresentationSize(1280, 720);
  EXPECT_TRUE(render_->submitted.empty());
  service_->SetPresentationSinkId(99);

  ASSERT_EQ(render_->submitted.size(), 1u);
  EXPECT_EQ(render_->submitted.front().presentation_sink_id, 99u);
  EXPECT_EQ(render_->submitted.front().requested_width, 1280);
  EXPECT_EQ(render_->submitted.front().requested_height, 720);
  EXPECT_NE(service_->first_frame_request_id(), 0u);
}

TEST_F(EditorSessionServiceTest, InitialRenderSchedulingFailureDoesNotStayLoading) {
  render_->fail_submit = true;

  const auto result    = service_->Open(100, 200);

  EXPECT_EQ(result.kind, EditorSessionResultKind::Failed);
  EXPECT_EQ(service_->state(), EditorSessionState::Failed);
  EXPECT_EQ(service_->first_frame_request_id(), 0u);
}

TEST_F(EditorSessionServiceTest, ImageAcquireAloneDoesNotLeaveLoading) {
  service_->Open(1, 2);
  // Open already marks the image acquired after guards; re-notify must not jump state.
  service_->NotifyImageAcquired(service_->identity().session_generation, true);
  EXPECT_EQ(service_->state(), EditorSessionState::Loading);
  EXPECT_NE(service_->first_frame_request_id(), 0u);
}

TEST_F(EditorSessionServiceTest, OpenMarksImageAcquiredButStaysLoadingUntilFirstFrame) {
  service_->Open(1, 2);
  EXPECT_EQ(service_->state(), EditorSessionState::Loading);
  EXPECT_NE(service_->first_frame_request_id(), 0u);
  // Guards succeeded: acquire is done without a separate NotifyImageAcquired call.
  PresentFirstFrame(*service_);
  EXPECT_EQ(service_->state(), EditorSessionState::Interactive);
}

TEST_F(EditorSessionServiceTest, QualityBaseFollowsInteractivePrimaryFirstFrame) {
  service_->Open(1, 2);
  ASSERT_EQ(render_->submitted.size(), 1u);
  EXPECT_EQ(render_->submitted.front().reason, EditorRenderReason::InitialFrame);
  EXPECT_EQ(render_->submitted.front().frame_role, FrameRole::InteractivePrimary);
  EXPECT_EQ(render_->submitted.front().quality, EditorRenderQuality::Interactive);

  PresentFirstFrame(*service_);
  EXPECT_EQ(service_->state(), EditorSessionState::Interactive);
  ASSERT_GE(render_->submitted.size(), 2u);
  EXPECT_EQ(render_->submitted.back().frame_role, FrameRole::QualityBase);
  EXPECT_EQ(render_->submitted.back().quality, EditorRenderQuality::Quality);
  EXPECT_EQ(render_->submitted.back().replacement_key, "quality");
}

TEST_F(EditorSessionServiceTest, StateMachineIgnoresReorderedStaleCompletions) {
  service_->Open(1, 2);
  const auto gen_a = service_->identity().session_generation;
  PresentFirstFrame(*service_);
  EXPECT_EQ(service_->state(), EditorSessionState::Interactive);

  service_->Switch(3, 4);
  const auto gen_b = service_->identity().session_generation;
  EXPECT_NE(gen_a, gen_b);
  EXPECT_EQ(service_->state(), EditorSessionState::Loading);

  service_->NotifySaveFinished(gen_a, false, "stale save failure");
  service_->NotifyImageAcquired(gen_a, false, "stale load failure");
  EXPECT_EQ(service_->state(), EditorSessionState::Loading);
  EXPECT_EQ(service_->identity().element_id, 3u);

  PresentFirstFrame(*service_);
  EXPECT_EQ(service_->state(), EditorSessionState::Interactive);
}

TEST_F(EditorSessionServiceTest, SwitchCancelsPriorSessionAndSealsJournal) {
  service_->Open(1, 2);
  PresentFirstFrame(*service_);
  const auto gen_a = service_->identity().session_generation;
  service_->Switch(9, 10);
  ASSERT_FALSE(render_->cancelled_sessions.empty());
  EXPECT_EQ(render_->cancelled_sessions.front(), gen_a);
  ASSERT_FALSE(render_->waited_sessions.empty());
  EXPECT_EQ(render_->waited_sessions.front(), gen_a);
  EXPECT_GE(journal_->barrier_count, 1);
  EXPECT_GE(tasks_->begin_count, 1);
  ASSERT_FALSE(render_->submitted.empty());
  EXPECT_EQ(render_->submitted.back().reason, EditorRenderReason::ImageSwitch);

  bool saw_save_started = false;
  for (const auto& r : service_->results()) {
    if (r.kind == EditorSessionResultKind::SaveStarted) {
      saw_save_started = true;
      EXPECT_NE(r.task_id, 0u);
    }
  }
  EXPECT_TRUE(saw_save_started);
}

TEST_F(EditorSessionServiceTest, ConcurrentSavesForAThenBFinishInEitherOrder) {
  service_->Open(1, 2);
  PresentFirstFrame(*service_);
  const auto gen_a = service_->identity().session_generation;

  service_->Switch(3, 4);
  PresentFirstFrame(*service_);
  const auto gen_b = service_->identity().session_generation;

  service_->Switch(5, 6);
  const auto gen_c = service_->identity().session_generation;
  EXPECT_NE(gen_a, gen_b);
  EXPECT_NE(gen_b, gen_c);
  EXPECT_EQ(tasks_->begin_count, 2);

  // Finish B then A (out of order).
  service_->NotifySaveFinished(gen_b, true, "B ok");
  service_->NotifySaveFinished(gen_a, true, "A ok");
  ASSERT_EQ(tasks_->ended_ids.size(), 2u);
  EXPECT_EQ(tasks_->ended_ids[0], tasks_->begun_ids[1]);
  EXPECT_EQ(tasks_->ended_ids[1], tasks_->begun_ids[0]);
  EXPECT_EQ(service_->state(), EditorSessionState::Loading);
}

TEST_F(EditorSessionServiceTest, ConcurrentSavesFinishInOpenOrder) {
  service_->Open(1, 2);
  PresentFirstFrame(*service_);
  const auto gen_a = service_->identity().session_generation;
  service_->Switch(3, 4);
  PresentFirstFrame(*service_);
  const auto gen_b = service_->identity().session_generation;
  service_->Switch(5, 6);

  service_->NotifySaveFinished(gen_a, true, "A ok");
  service_->NotifySaveFinished(gen_b, true, "B ok");
  ASSERT_EQ(tasks_->ended_ids.size(), 2u);
  EXPECT_EQ(tasks_->ended_ids[0], tasks_->begun_ids[0]);
  EXPECT_EQ(tasks_->ended_ids[1], tasks_->begun_ids[1]);
}

TEST_F(EditorSessionServiceTest, PatchAndGestureCommitRouteThroughRenderPortOnly) {
  service_->Open(1, 2);
  PresentFirstFrame(*service_);
  render_->submitted.clear();

  EditorAdjustmentPatch patch;
  patch.field_key   = "exposure";
  patch.params_json = R"({"exposure":1.0})";
  service_->Patch(patch);
  ASSERT_EQ(render_->submitted.size(), 1u);
  EXPECT_EQ(render_->submitted.back().reason, EditorRenderReason::InteractiveAdjustment);
  EXPECT_EQ(render_->submitted.back().adjustment.params_json, R"({"exposure":1.0})");
  ASSERT_FALSE(render_->submitted.back().adjustment.patches.empty());
  EXPECT_EQ(render_->submitted.back().adjustment.patches.back().field_key, "exposure");

  patch.settled = true;
  service_->GestureCommit(patch);
  ASSERT_EQ(render_->submitted.size(), 2u);
  EXPECT_EQ(render_->submitted.back().reason, EditorRenderReason::SettledAdjustment);
}

TEST_F(EditorSessionServiceTest, PatchWhileLoadingIsRejectedWithoutCancellingFirstFrame) {
  service_->Open(1, 2);
  const auto first_request = service_->first_frame_request_id();
  ASSERT_NE(first_request, 0u);

  EditorAdjustmentPatch patch;
  patch.field_key   = "exposure";
  const auto result = service_->Patch(patch);

  EXPECT_EQ(result.kind, EditorSessionResultKind::Rejected);
  EXPECT_EQ(service_->state(), EditorSessionState::Loading);
  EXPECT_EQ(service_->first_frame_request_id(), first_request);
  ASSERT_EQ(render_->submitted.size(), 1u);

  PresentFirstFrame(*service_);
  EXPECT_EQ(service_->state(), EditorSessionState::Interactive);
}

TEST_F(EditorSessionServiceTest, SwitchingImagesDoesNotReuseThePreviousAdjustment) {
  service_->Open(1, 2);
  PresentFirstFrame(*service_);

  EditorAdjustmentPatch patch;
  patch.field_key   = "exposure";
  patch.params_json = R"({"exposure":1.0})";
  service_->Patch(patch);
  ASSERT_FALSE(service_->adjustment_snapshot().params_json.empty());

  service_->Switch(3, 4);

  ASSERT_FALSE(render_->submitted.empty());
  const auto& switched = render_->submitted.back();
  EXPECT_EQ(switched.reason, EditorRenderReason::ImageSwitch);
  EXPECT_TRUE(switched.adjustment.params_json.empty());
  EXPECT_TRUE(switched.adjustment.patches.empty());
  EXPECT_TRUE(service_->adjustment_snapshot().params_json.empty());
}

TEST_F(EditorSessionServiceTest, UndoRedoAdvanceRenderGenerationAndRoute) {
  service_->Open(1, 2);
  PresentFirstFrame(*service_);
  const auto render_before = service_->identity().render_generation;
  render_->submitted.clear();
  service_->Undo();
  EXPECT_EQ(history_->undo_count, 1);
  EXPECT_GT(service_->identity().render_generation, render_before);
  ASSERT_EQ(render_->submitted.size(), 1u);
  EXPECT_EQ(render_->submitted.back().reason, EditorRenderReason::UndoRedo);
}

TEST_F(EditorSessionServiceTest, UndoAndRedoRenderTheSnapshotReturnedByHistory) {
  service_->Open(1, 2);
  PresentFirstFrame(*service_);
  render_->submitted.clear();

  history_->current_snapshot.params_json = R"({"exposure":-0.5})";
  history_->current_snapshot.fingerprint = "undo-state";
  service_->Undo();
  ASSERT_EQ(render_->submitted.size(), 1u);
  EXPECT_EQ(render_->submitted.back().adjustment.params_json, R"({"exposure":-0.5})");

  history_->current_snapshot.params_json = R"({"exposure":0.75})";
  history_->current_snapshot.fingerprint = "redo-state";
  service_->Redo();
  ASSERT_EQ(render_->submitted.size(), 2u);
  EXPECT_EQ(render_->submitted.back().adjustment.params_json, R"({"exposure":0.75})");
}

TEST_F(EditorSessionServiceTest, DiscardUsesJournalPort) {
  service_->Open(1, 2);
  PresentFirstFrame(*service_);
  history_->current_snapshot.params_json = R"({"contrast":0.0})";
  service_->Discard();
  EXPECT_EQ(journal_->discard_count, 1);
  EXPECT_EQ(render_->submitted.back().adjustment.params_json, R"({"contrast":0.0})");
}

TEST_F(EditorSessionServiceTest, DiscardCannotTurnAnAcquireFailureInteractive) {
  pipeline_->fail_acquire = true;
  service_->Open(1, 2);
  ASSERT_EQ(service_->state(), EditorSessionState::Failed);

  const auto discarded = service_->Discard();

  EXPECT_EQ(discarded.kind, EditorSessionResultKind::Rejected);
  EXPECT_EQ(service_->state(), EditorSessionState::Failed);
  EXPECT_TRUE(render_->submitted.empty());
}

TEST_F(EditorSessionServiceTest, DiscardRetryFailureReturnsToFailedState) {
  service_->Open(1, 2);
  PresentFirstFrame(*service_);
  render_->fail_submit = true;

  EditorAdjustmentPatch patch;
  patch.field_key = "exposure";
  ASSERT_EQ(service_->Patch(patch).kind, EditorSessionResultKind::Failed);
  ASSERT_EQ(service_->state(), EditorSessionState::Failed);

  const auto discarded = service_->Discard();

  EXPECT_EQ(discarded.kind, EditorSessionResultKind::Failed);
  EXPECT_EQ(service_->state(), EditorSessionState::Failed);
}

TEST_F(EditorSessionServiceTest, CloseCanPersistOrDiscardThroughTheSameLifecycleOwner) {
  service_->Open(1, 2);
  PresentFirstFrame(*service_);
  const auto first_session = service_->identity().session_generation;

  const auto discarded     = service_->Close(false);
  EXPECT_EQ(discarded.kind, EditorSessionResultKind::StateChanged);
  EXPECT_EQ(service_->state(), EditorSessionState::NoImage);
  EXPECT_EQ(journal_->discard_count, 1);
  ASSERT_FALSE(render_->cancelled_sessions.empty());
  EXPECT_EQ(render_->cancelled_sessions.back(), first_session);
  ASSERT_FALSE(render_->waited_sessions.empty());
  EXPECT_EQ(render_->waited_sessions.back(), first_session);

  service_->Open(3, 4);
  const auto persisted = service_->Close(true);
  EXPECT_EQ(persisted.kind, EditorSessionResultKind::StateChanged);
  EXPECT_EQ(journal_->barrier_count, 1);
  EXPECT_EQ(tasks_->begin_count, 1);
}

TEST_F(EditorSessionServiceTest, SwitchStopsWhenJournalOrSaveTaskCannotStart) {
  service_->Open(1, 2);
  PresentFirstFrame(*service_);

  journal_->fail_barrier     = true;
  const auto journal_failure = service_->Switch(3, 4);
  EXPECT_EQ(journal_failure.kind, EditorSessionResultKind::Failed);
  EXPECT_EQ(service_->identity().element_id, 1u);
  EXPECT_EQ(pipeline_->release_count, 0);

  journal_->fail_barrier  = false;
  tasks_->fail_begin      = true;
  const auto task_failure = service_->Switch(3, 4);
  EXPECT_EQ(task_failure.kind, EditorSessionResultKind::Failed);
  EXPECT_EQ(service_->identity().element_id, 1u);
  EXPECT_EQ(pipeline_->release_count, 0);
}

TEST_F(EditorSessionServiceTest, SaveStartedIsReportedWhileStateIsSaving) {
  service_->Open(1, 2);
  PresentFirstFrame(*service_);
  std::vector<EditorSessionState> save_states;
  service_->SetResultObserver([&](const EditorSessionResult& result) {
    if (result.kind == EditorSessionResultKind::SaveStarted) {
      save_states.push_back(result.state);
    }
  });

  service_->Switch(3, 4);

  ASSERT_EQ(save_states.size(), 1u);
  EXPECT_EQ(save_states.front(), EditorSessionState::Saving);
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
  const auto result       = service_->Open(1, 2);
  EXPECT_EQ(service_->state(), EditorSessionState::Failed);
  EXPECT_EQ(result.kind, EditorSessionResultKind::Failed);
  EXPECT_TRUE(render_->submitted.empty());
}

TEST_F(EditorSessionServiceTest, FramePresentedMovesLoadingToInteractiveOnlyWhenMatching) {
  service_->Open(1, 2);
  EXPECT_EQ(service_->state(), EditorSessionState::Loading);
  const auto first_id = service_->first_frame_request_id();
  ASSERT_NE(first_id, 0u);

  service_->NotifyImageAcquired(service_->identity().session_generation, true);
  EXPECT_EQ(service_->state(), EditorSessionState::Loading);

  // Stale older render generation must not complete the first-frame gate.
  EditorRenderResult stale_presented;
  stale_presented.kind                     = EditorRenderResultKind::FramePresented;
  stale_presented.request_id               = first_id;
  stale_presented.intent                   = render_->submitted.front();
  stale_presented.intent.render_generation = 999;
  service_->NotifyRenderResult(stale_presented);
  EXPECT_EQ(service_->state(), EditorSessionState::Loading);

  // Ordered complete → submit → present for the matching first-frame request.
  EditorRenderResult completed;
  completed.kind       = EditorRenderResultKind::RenderCompleted;
  completed.request_id = first_id;
  completed.intent     = render_->submitted.front();
  service_->NotifyRenderResult(completed);
  EXPECT_EQ(service_->state(), EditorSessionState::Loading);

  EditorRenderResult submitted;
  submitted.kind       = EditorRenderResultKind::FrameSubmitted;
  submitted.request_id = first_id;
  submitted.intent     = render_->submitted.front();
  service_->NotifyRenderResult(submitted);

  EditorRenderResult presented;
  presented.kind       = EditorRenderResultKind::FramePresented;
  presented.request_id = first_id;
  presented.intent     = render_->submitted.front();
  service_->NotifyRenderResult(presented);
  EXPECT_EQ(service_->state(), EditorSessionState::Interactive);

  // Duplicate presented is ignored.
  service_->NotifyRenderResult(presented);
  EXPECT_EQ(service_->state(), EditorSessionState::Interactive);
}

TEST_F(EditorSessionServiceTest, LateOldRenderInSameSessionIsIgnored) {
  service_->Open(1, 2);
  PresentFirstFrame(*service_);
  EXPECT_EQ(service_->state(), EditorSessionState::Interactive);

  // Patch advances render generation.
  service_->Patch("exposure");
  const auto         new_render_gen = service_->identity().render_generation;

  EditorRenderResult late_old;
  late_old.kind                     = EditorRenderResultKind::FramePresented;
  late_old.request_id               = 999;
  late_old.intent                   = render_->submitted.front();
  late_old.intent.render_generation = new_render_gen - 1;
  service_->NotifyRenderResult(late_old);
  EXPECT_EQ(service_->state(), EditorSessionState::Interactive);
  EXPECT_EQ(service_->identity().render_generation, new_render_gen);
}

TEST_F(EditorSessionServiceTest, ReopenSameImageIsNoOpWithoutLeakingGuards) {
  service_->Open(1, 2);
  const auto gen1 = service_->identity().session_generation;
  EXPECT_EQ(pipeline_->acquire_count, 1);
  EXPECT_EQ(pipeline_->release_count, 0);

  const auto second = service_->Open(1, 2);
  EXPECT_EQ(second.kind, EditorSessionResultKind::Accepted);
  EXPECT_EQ(service_->identity().session_generation, gen1);
  EXPECT_EQ(pipeline_->acquire_count, 1);
  EXPECT_EQ(pipeline_->release_count, 0);
  EXPECT_TRUE(render_->cancelled_sessions.empty());
}

TEST_F(EditorSessionServiceTest, WorksWithRealCoordinatorAsRenderPort) {
  auto scheduler   = std::make_shared<EditorSessionBootstrapSchedulerPort>();
  auto coordinator = std::make_shared<EditorRenderCoordinator>(scheduler);
  EditorSessionService::Dependencies deps;
  deps.pipeline = pipeline_;
  deps.history  = history_;
  deps.tasks    = tasks_;
  deps.journal  = journal_;
  deps.render   = coordinator;
  EditorSessionService service(std::move(deps));
  service.SetPresentationSinkId(1);
  service.SetPresentationSize(640, 480);
  service.Open(7, 8);
  EXPECT_EQ(service.state(), EditorSessionState::Loading);
  EXPECT_EQ(scheduler->scheduled().size(), 1u);
  EXPECT_EQ(scheduler->scheduled().front().intent.reason, EditorRenderReason::InitialFrame);
}

TEST_F(EditorSessionServiceTest, RuntimeForwardsCoordinatorResultsToControllerState) {
  auto runtime      = EditorSessionRuntime::Create();
  int  change_count = 0;
  runtime->service->SetChangeNotifier([&] { ++change_count; });
  runtime->service->SetPresentationSinkId(1);
  runtime->service->SetPresentationSize(640, 480);

  runtime->service->Open(11, 22);
  EXPECT_EQ(runtime->service->state(), EditorSessionState::Loading);
  auto* bootstrap_scheduler =
      dynamic_cast<EditorSessionBootstrapSchedulerPort*>(runtime->scheduler.get());
  ASSERT_NE(bootstrap_scheduler, nullptr);
  ASSERT_EQ(bootstrap_scheduler->scheduled().size(), 1u);
  const auto request_id = bootstrap_scheduler->scheduled().front().request_id;

  runtime->service->NotifyImageAcquired(runtime->service->identity().session_generation, true);
  EXPECT_EQ(runtime->service->state(), EditorSessionState::Loading);

  // Drive coordinator completion → observer → service NotifyRenderResult.
  runtime->coordinator->NotifySchedulerCompleted(request_id, true);
  runtime->coordinator->NotifyFrameSubmitted(request_id);
  runtime->coordinator->NotifyFramePresented(request_id);

  EXPECT_EQ(runtime->service->state(), EditorSessionState::Interactive);
  EXPECT_GT(change_count, 0);
}

TEST_F(EditorSessionServiceTest, ScheduledAdjustmentMatchesSubmittedSnapshotFieldByField) {
  service_->Open(1, 2);
  PresentFirstFrame(*service_);
  render_->submitted.clear();

  EditorAdjustmentPatch patch;
  patch.field_key   = "contrast";
  patch.params_json = R"({"contrast":0.25})";
  service_->Patch(patch);

  ASSERT_EQ(render_->submitted.size(), 1u);
  const auto& snap = render_->submitted.back().adjustment;
  EXPECT_EQ(snap.params_json, R"({"contrast":0.25})");
  ASSERT_EQ(snap.patches.size(), 1u);
  EXPECT_EQ(snap.patches[0].field_key, "contrast");
  EXPECT_EQ(snap.patches[0].params_json, R"({"contrast":0.25})");
  EXPECT_FALSE(snap.patches[0].settled);
  EXPECT_FALSE(snap.fingerprint.empty());
}

// ---------------------------------------------------------------------------
// Phase 5D: ViewChange intent routing through the real coordinator (A1/A2/A3,
// render_busy for D6). These use EditorSessionRuntime so the coordinator's
// reuse-vs-render decision is exercised end-to-end through the service.

TEST_F(EditorSessionServiceTest, ViewChangeZoomPanIsReusedWithoutScheduling) {
  auto runtime = EditorSessionRuntime::Create();
  DriveRuntimeToInteractive(*runtime);
  auto* sched = dynamic_cast<EditorSessionBootstrapSchedulerPort*>(runtime->scheduler.get());
  ASSERT_NE(sched, nullptr);
  const auto scheduled_before = sched->scheduled().size();

  const auto result =
      runtime->service->RequestViewChange(EditorRenderReason::ZoomPan, std::nullopt);
  // The coordinator reused the current full frame (D2); the service maps Reused
  // to Accepted. No new pipeline task, no busy state.
  EXPECT_EQ(result.kind, EditorSessionResultKind::Accepted);
  EXPECT_EQ(sched->scheduled().size(), scheduled_before);
  EXPECT_FALSE(runtime->service->render_busy());
}

TEST_F(EditorSessionServiceTest, ViewChangeResizeIsReusedWithoutScheduling) {
  auto runtime = EditorSessionRuntime::Create();
  DriveRuntimeToInteractive(*runtime);
  auto* sched = dynamic_cast<EditorSessionBootstrapSchedulerPort*>(runtime->scheduler.get());
  ASSERT_NE(sched, nullptr);
  const auto scheduled_before = sched->scheduled().size();

  const auto result = runtime->service->RequestViewChange(EditorRenderReason::Resize, std::nullopt);
  EXPECT_EQ(result.kind, EditorSessionResultKind::Accepted);
  EXPECT_EQ(sched->scheduled().size(), scheduled_before);
  EXPECT_FALSE(runtime->service->render_busy());
}

TEST_F(EditorSessionServiceTest, ViewChangeDetailRefreshSchedulesDetailPatch) {
  auto runtime = EditorSessionRuntime::Create();
  DriveRuntimeToInteractive(*runtime);
  auto* sched = dynamic_cast<EditorSessionBootstrapSchedulerPort*>(runtime->scheduler.get());
  ASSERT_NE(sched, nullptr);

  const auto region = MakeRegion(7);
  const auto result =
      runtime->service->RequestViewChange(EditorRenderReason::DetailRefresh, region);
  EXPECT_EQ(result.kind, EditorSessionResultKind::RenderRouted);
  ASSERT_FALSE(sched->scheduled().empty());
  const auto& scheduled = sched->scheduled().back().intent;
  EXPECT_EQ(scheduled.frame_role, FrameRole::DetailPatch);
  EXPECT_EQ(scheduled.reason, EditorRenderReason::DetailRefresh);
  EXPECT_EQ(scheduled.quality, EditorRenderQuality::Detail);
  ASSERT_TRUE(scheduled.view_region.has_value());
  EXPECT_EQ(scheduled.view_region->x_, 7);
  EXPECT_TRUE(runtime->service->render_busy());
}

TEST_F(EditorSessionServiceTest, ViewChangeCropRotateSchedulesInteractivePrimary) {
  auto runtime = EditorSessionRuntime::Create();
  DriveRuntimeToInteractive(*runtime);
  auto* sched = dynamic_cast<EditorSessionBootstrapSchedulerPort*>(runtime->scheduler.get());
  ASSERT_NE(sched, nullptr);
  const auto render_gen_before = runtime->service->identity().render_generation;

  const auto result =
      runtime->service->RequestViewChange(EditorRenderReason::CropRotate, std::nullopt);
  EXPECT_EQ(result.kind, EditorSessionResultKind::RenderRouted);
  // A content-changing geometry change advances the render generation.
  EXPECT_GT(runtime->service->identity().render_generation, render_gen_before);
  ASSERT_FALSE(sched->scheduled().empty());
  const auto& scheduled = sched->scheduled().back().intent;
  EXPECT_EQ(scheduled.frame_role, FrameRole::InteractivePrimary);
  EXPECT_EQ(scheduled.reason, EditorRenderReason::CropRotate);
  EXPECT_EQ(scheduled.quality, EditorRenderQuality::Interactive);
  EXPECT_FALSE(scheduled.view_region.has_value());
  EXPECT_TRUE(runtime->service->render_busy());
}

TEST_F(EditorSessionServiceTest, ViewChangeRejectedWhenNotInteractive) {
  auto runtime = EditorSessionRuntime::Create();
  // No Open: the session is NoImage, so a view change cannot be routed.
  const auto result =
      runtime->service->RequestViewChange(EditorRenderReason::ZoomPan, std::nullopt);
  EXPECT_EQ(result.kind, EditorSessionResultKind::Rejected);
  EXPECT_FALSE(runtime->service->render_busy());
}

TEST_F(EditorSessionServiceTest, ViewChangeBurstKeepsNewestDetailAndCancelsPrior) {
  // Phase 5D A2: a burst of replaceable view-change input (here, repeated ROI
  // detail refreshes as a pinch-zoom burst) does not let an outdated detail
  // patch complete. Each refresh advances the view generation and cancels the
  // prior in-flight detail patch; only the newest survives (last state never
  // lost) and the coordinator goes idle once it completes.
  auto runtime = EditorSessionRuntime::Create();
  DriveRuntimeToInteractive(*runtime);
  auto* sched = dynamic_cast<EditorSessionBootstrapSchedulerPort*>(runtime->scheduler.get());
  ASSERT_NE(sched, nullptr);

  std::uint64_t last_request_id = 0;
  for (int i = 0; i < 6; ++i) {
    const auto result = runtime->service->RequestViewChange(EditorRenderReason::DetailRefresh,
                                                            MakeRegion(i));
    ASSERT_EQ(result.kind, EditorSessionResultKind::RenderRouted);
    last_request_id = result.render_request_id;
  }
  EXPECT_TRUE(runtime->service->render_busy());
  // The newest detail patch is the in-flight one; prior patches were cancelled.
  EXPECT_EQ(runtime->coordinator->last_scheduled_request_id(), last_request_id);
  EXPECT_EQ(sched->cancelled().size(), 5u);

  // Completing the newest detail patch drains the coordinator.
  runtime->coordinator->NotifySchedulerCompleted(last_request_id, true);
  EXPECT_FALSE(runtime->service->render_busy());
}

TEST_F(EditorSessionServiceTest, RenderBusyTransitionsAroundViewChange) {
  // Phase 5D D6: render_busy reflects coordinator diagnostics only and
  // transitions on submit / completion via the backend notifier.
  auto runtime = EditorSessionRuntime::Create();
  DriveRuntimeToInteractive(*runtime);
  EXPECT_FALSE(runtime->service->render_busy());

  const auto result =
      runtime->service->RequestViewChange(EditorRenderReason::DetailRefresh, MakeRegion(3));
  ASSERT_EQ(result.kind, EditorSessionResultKind::RenderRouted);
  EXPECT_TRUE(runtime->service->render_busy());

  runtime->coordinator->NotifySchedulerCompleted(result.render_request_id, true);
  EXPECT_FALSE(runtime->service->render_busy());
}

}  // namespace
}  // namespace alcedo
