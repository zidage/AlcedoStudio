//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_render_coordinator.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace alcedo {
namespace {

class RecordingScheduler final : public IEditorPipelineSchedulerPort {
 public:
  auto Schedule(const EditorRenderRequest& request) -> std::uint64_t override {
    if (fail_next_) {
      fail_next_ = false;
      return 0;
    }
    scheduled_.push_back(request);
    return ++next_job_;
  }
  void Cancel(std::uint64_t job_id) override { cancelled_.push_back(job_id); }
  void WaitForSessionIdle(std::uint64_t session_generation) override {
    waited_sessions_.push_back(session_generation);
  }

  std::vector<EditorRenderRequest> scheduled_;
  std::vector<std::uint64_t>       cancelled_;
  std::vector<std::uint64_t>       waited_sessions_;
  std::uint64_t                    next_job_  = 0;
  bool                             fail_next_ = false;
};

auto MakeIntent(EditorRenderQuality quality, EditorRenderPriority priority,
                std::uint64_t session_gen = 1, std::uint64_t render_gen = 1,
                std::uint64_t view_gen = 1) -> EditorRenderIntent {
  EditorRenderIntent intent;
  intent.element_id         = 10;
  intent.image_id           = 20;
  intent.image_load_request_id = ImageLoadRequestId{session_gen};
  intent.render_generation  = render_gen;
  intent.view_generation    = view_gen;
  intent.quality            = quality;
  intent.priority           = priority;
  intent.frame_role         = FrameRoleForQuality(quality);
  intent.replacement_key    = DefaultReplacementKey(quality);
  intent.reason             = EditorRenderReason::InteractiveAdjustment;
  return intent;
}

// Phase 5D: view-change intent (zoom/pan/resize/crop-rotation/ROI). Carries the
// reason that drives the coordinator's reuse-vs-render decision; quality still
// sets frame_role/replacement_key like a service-produced intent.
auto MakeViewIntent(EditorRenderReason reason, EditorRenderQuality quality,
                    EditorRenderPriority priority = EditorRenderPriority::Normal,
                    std::uint64_t session_gen = 1, std::uint64_t render_gen = 1,
                    std::uint64_t view_gen = 1) -> EditorRenderIntent {
  EditorRenderIntent intent = MakeIntent(quality, priority, session_gen, render_gen, view_gen);
  intent.reason             = reason;
  return intent;
}

class EditorRenderCoordinatorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    scheduler_   = std::make_shared<RecordingScheduler>();
    coordinator_ = std::make_unique<EditorRenderCoordinator>(scheduler_);
    coordinator_->SetActiveGenerations(1, 1, 1);
  }

  std::shared_ptr<RecordingScheduler>      scheduler_;
  std::unique_ptr<EditorRenderCoordinator> coordinator_;
};

TEST_F(EditorRenderCoordinatorTest, AcceptsIntentAndSchedulesThroughSingleOwner) {
  const auto accepted = coordinator_->Submit(
      MakeIntent(EditorRenderQuality::Interactive, EditorRenderPriority::Normal));
  EXPECT_EQ(accepted.kind, EditorRenderResultKind::RequestAccepted);
  ASSERT_EQ(scheduler_->scheduled_.size(), 1u);
  EXPECT_EQ(scheduler_->scheduled_.front().request_id, accepted.request_id);
  EXPECT_TRUE(coordinator_->has_inflight());
}

TEST_F(EditorRenderCoordinatorTest, RejectsStaleImageLoadRequest) {
  // MakeIntent's first generation arg stamps image_load_request_id; active is 1.
  const auto rejected = coordinator_->Submit(
      MakeIntent(EditorRenderQuality::Interactive, EditorRenderPriority::Normal, /*session=*/2));
  EXPECT_EQ(rejected.kind, EditorRenderResultKind::Failed);
  EXPECT_TRUE(scheduler_->scheduled_.empty());
  EXPECT_NE(rejected.message.find("image load request"), std::string::npos);
}

TEST_F(EditorRenderCoordinatorTest, ReplacesPendingWorkWithSameReplacementKey) {
  auto first = coordinator_->Submit(
      MakeIntent(EditorRenderQuality::Interactive, EditorRenderPriority::Normal));
  ASSERT_TRUE(coordinator_->has_inflight());

  // Phase 5D: ZoomPan/Resize are reused (never enqueued), so a replacement-key
  // test must use a reason that enqueues. MakeIntent defaults to
  // InteractiveAdjustment, which produces a pending "interactive" entry that a
  // newer same-key submit replaces.
  auto       older       = MakeIntent(EditorRenderQuality::Interactive, EditorRenderPriority::Low);
  const auto pending_old = coordinator_->Submit(older);
  EXPECT_EQ(pending_old.kind, EditorRenderResultKind::RequestAccepted);
  EXPECT_EQ(coordinator_->pending_count(), 1u);

  auto       newer       = MakeIntent(EditorRenderQuality::Interactive, EditorRenderPriority::High);
  const auto pending_new = coordinator_->Submit(newer);
  EXPECT_EQ(pending_new.kind, EditorRenderResultKind::RequestAccepted);
  EXPECT_EQ(coordinator_->pending_count(), 1u);

  bool saw_replaced = false;
  for (const auto& result : coordinator_->results()) {
    if (result.kind == EditorRenderResultKind::Replaced &&
        result.request_id == pending_old.request_id) {
      saw_replaced = true;
    }
  }
  EXPECT_TRUE(saw_replaced);
  EXPECT_EQ(first.kind, EditorRenderResultKind::RequestAccepted);
}

TEST_F(EditorRenderCoordinatorTest, SchedulesInteractiveBeforeQualityWhenBothPending) {
  // Phase 5D D4 priority order for visible work: missing first frame / current
  // interactive response (InteractivePrimary) first, then settled QualityBase,
  // then the current detail patch, then background. Interactive work is never
  // blocked behind an outdated quality or detail request. Role rank dominates
  // EditorRenderPriority, so Interactive (role 3) outranks Quality (role 2)
  // even when Quality carries the higher priority value.
  coordinator_->Submit(MakeIntent(EditorRenderQuality::Detail, EditorRenderPriority::Low));
  ASSERT_TRUE(coordinator_->has_inflight());
  const auto inflight_id = coordinator_->last_scheduled_request_id();

  const auto interactive = coordinator_->Submit(
      MakeIntent(EditorRenderQuality::Interactive, EditorRenderPriority::Normal));
  const auto quality =
      coordinator_->Submit(MakeIntent(EditorRenderQuality::Quality, EditorRenderPriority::High));
  EXPECT_EQ(coordinator_->pending_count(), 2u);

  coordinator_->NotifySchedulerCompleted(inflight_id, true);
  ASSERT_EQ(scheduler_->scheduled_.size(), 2u);
  EXPECT_EQ(scheduler_->scheduled_.back().request_id, interactive.request_id);
  EXPECT_NE(scheduler_->scheduled_.back().request_id, quality.request_id);
}

TEST_F(EditorRenderCoordinatorTest, CancelSessionDropsPendingAndInflight) {
  auto a = coordinator_->Submit(
      MakeIntent(EditorRenderQuality::Interactive, EditorRenderPriority::Normal));
  auto b = MakeIntent(EditorRenderQuality::Quality, EditorRenderPriority::Normal);
  coordinator_->Submit(b);
  EXPECT_TRUE(coordinator_->has_inflight());
  EXPECT_EQ(coordinator_->pending_count(), 1u);

  coordinator_->CancelSession(1);
  EXPECT_FALSE(coordinator_->has_inflight());
  EXPECT_EQ(coordinator_->pending_count(), 0u);
  EXPECT_FALSE(scheduler_->cancelled_.empty());
  EXPECT_EQ(a.kind, EditorRenderResultKind::RequestAccepted);
}

TEST_F(EditorRenderCoordinatorTest, CancelSessionAndWaitJoinsTheMatchingSchedulerWork) {
  coordinator_->Submit(MakeIntent(EditorRenderQuality::Interactive, EditorRenderPriority::Normal));

  coordinator_->CancelSessionAndWait(1);

  ASSERT_EQ(scheduler_->cancelled_.size(), 1u);
  ASSERT_EQ(scheduler_->waited_sessions_.size(), 1u);
  EXPECT_EQ(scheduler_->waited_sessions_.front(), 1u);
  EXPECT_FALSE(coordinator_->has_inflight());
}

TEST_F(EditorRenderCoordinatorTest, CancellationTokenPreventsSchedule) {
  coordinator_->Submit(MakeIntent(EditorRenderQuality::Interactive, EditorRenderPriority::Normal));
  auto intent         = MakeIntent(EditorRenderQuality::Quality, EditorRenderPriority::High);
  intent.cancellation = std::make_shared<EditorRenderCancellationToken>();
  const auto accepted = coordinator_->Submit(intent);
  EXPECT_EQ(accepted.kind, EditorRenderResultKind::RequestAccepted);
  intent.cancellation->Cancel();

  coordinator_->NotifySchedulerCompleted(coordinator_->last_scheduled_request_id(), true);
  bool scheduled_quality = false;
  for (const auto& req : scheduler_->scheduled_) {
    if (req.request_id == accepted.request_id) {
      scheduled_quality = true;
    }
  }
  EXPECT_FALSE(scheduled_quality);
}

TEST_F(EditorRenderCoordinatorTest, CancellationTokenStopsAnInflightRequestAndStartsNextWork) {
  auto inflight_intent = MakeIntent(EditorRenderQuality::Interactive, EditorRenderPriority::Normal);
  inflight_intent.cancellation = std::make_shared<EditorRenderCancellationToken>();
  const auto inflight          = coordinator_->Submit(inflight_intent);
  ASSERT_TRUE(coordinator_->has_inflight());

  auto pending            = MakeIntent(EditorRenderQuality::Quality, EditorRenderPriority::Normal);
  pending.replacement_key = "quality-after-cancel";
  const auto pending_result = coordinator_->Submit(pending);
  ASSERT_EQ(coordinator_->pending_count(), 1u);

  inflight_intent.cancellation->Cancel();

  ASSERT_EQ(scheduler_->cancelled_.size(), 1u);
  EXPECT_TRUE(coordinator_->has_inflight());
  EXPECT_EQ(coordinator_->last_scheduled_request_id(), pending_result.request_id);
  EXPECT_EQ(coordinator_->pending_count(), 0u);

  int cancellation_results = 0;
  for (const auto& result : coordinator_->results()) {
    if (result.request_id == inflight.request_id &&
        result.kind == EditorRenderResultKind::Cancelled) {
      ++cancellation_results;
    }
  }
  EXPECT_EQ(cancellation_results, 1);
}

TEST_F(EditorRenderCoordinatorTest, ObserverRunsAfterQueueStateIsStable) {
  const auto inflight = coordinator_->Submit(
      MakeIntent(EditorRenderQuality::Interactive, EditorRenderPriority::Normal));
  bool observer_cancelled = false;
  coordinator_->SetResultObserver([&](const EditorRenderResult& result) {
    if (!observer_cancelled && result.kind == EditorRenderResultKind::RequestAccepted &&
        result.request_id != inflight.request_id) {
      observer_cancelled = coordinator_->CancelRequest(inflight.request_id);
    }
  });

  auto pending            = MakeIntent(EditorRenderQuality::Quality, EditorRenderPriority::High);
  pending.replacement_key = "observer-follow-up";
  const auto accepted     = coordinator_->Submit(pending);

  EXPECT_TRUE(observer_cancelled);
  ASSERT_EQ(scheduler_->cancelled_.size(), 1u);
  EXPECT_TRUE(coordinator_->has_inflight());
  EXPECT_EQ(coordinator_->last_scheduled_request_id(), accepted.request_id);
  EXPECT_EQ(coordinator_->pending_count(), 0u);
}

TEST_F(EditorRenderCoordinatorTest, ConcurrentSubmitReturnsWhileAnotherThreadIsBlockedInObserver) {
  std::mutex              observer_mutex;
  std::condition_variable observer_entered;
  std::condition_variable release_observer;
  bool                    observer_is_blocked = false;
  bool                    observer_can_return = false;

  coordinator_->SetResultObserver([&](const EditorRenderResult&) {
    std::unique_lock lock(observer_mutex);
    observer_is_blocked = true;
    observer_entered.notify_one();
    release_observer.wait(lock, [&] { return observer_can_return; });
  });

  std::thread first_submit([&] {
    coordinator_->Submit(
        MakeIntent(EditorRenderQuality::Interactive, EditorRenderPriority::Normal));
  });

  bool entered = false;
  {
    std::unique_lock lock(observer_mutex);
    entered = observer_entered.wait_for(lock, std::chrono::seconds(2),
                                        [&] { return observer_is_blocked; });
  }
  EXPECT_TRUE(entered);

  std::atomic<bool> second_returned = false;
  std::thread       second_submit([&] {
    coordinator_->Submit(
        MakeIntent(EditorRenderQuality::Interactive, EditorRenderPriority::Normal));
    second_returned.store(true, std::memory_order_release);
  });

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!second_returned.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  EXPECT_TRUE(second_returned.load(std::memory_order_acquire));

  {
    std::scoped_lock lock(observer_mutex);
    observer_can_return = true;
  }
  release_observer.notify_one();
  if (first_submit.joinable()) first_submit.join();
  if (second_submit.joinable()) second_submit.join();
}

TEST_F(EditorRenderCoordinatorTest, ObserverExceptionDoesNotDisableLaterDelivery) {
  coordinator_->SetResultObserver(
      [](const EditorRenderResult&) { throw std::runtime_error("observer failed"); });
  EXPECT_THROW(coordinator_->Submit(
                   MakeIntent(EditorRenderQuality::Interactive, EditorRenderPriority::Normal)),
               std::runtime_error);

  int delivered = 0;
  coordinator_->SetResultObserver([&](const EditorRenderResult&) { ++delivered; });
  auto next            = MakeIntent(EditorRenderQuality::Quality, EditorRenderPriority::Normal);
  next.replacement_key = "after-observer-failure";
  EXPECT_EQ(coordinator_->Submit(next).kind, EditorRenderResultKind::RequestAccepted);
  EXPECT_GT(delivered, 0);
}

TEST(EditorRenderCoordinatorLifetimeTest, LateTokenCancellationIgnoresDestroyedCoordinator) {
  auto scheduler = std::make_shared<RecordingScheduler>();
  auto token     = std::make_shared<EditorRenderCancellationToken>();
  {
    EditorRenderCoordinator coordinator(scheduler);
    coordinator.SetActiveGenerations(1, 1, 1);
    auto intent = MakeIntent(EditorRenderQuality::Interactive, EditorRenderPriority::Normal);
    intent.cancellation = token;
    ASSERT_EQ(coordinator.Submit(intent).kind, EditorRenderResultKind::RequestAccepted);
  }

  token->Cancel();
  SUCCEED();
}

TEST_F(EditorRenderCoordinatorTest, ReportsFramePresentationSeparateFromRenderCompletion) {
  const auto accepted = coordinator_->Submit(
      MakeIntent(EditorRenderQuality::Interactive, EditorRenderPriority::Normal));
  coordinator_->NotifySchedulerCompleted(accepted.request_id, true);
  coordinator_->NotifyFrameSubmitted(accepted.request_id);
  coordinator_->NotifyFramePresented(accepted.request_id);

  std::vector<EditorRenderResultKind> kinds;
  for (const auto& r : coordinator_->results()) {
    if (r.request_id == accepted.request_id) {
      kinds.push_back(r.kind);
    }
  }
  ASSERT_GE(kinds.size(), 4u);
  EXPECT_EQ(kinds[0], EditorRenderResultKind::RequestAccepted);
  EXPECT_EQ(kinds[1], EditorRenderResultKind::RenderStarted);
  EXPECT_EQ(kinds[2], EditorRenderResultKind::RenderCompleted);
  EXPECT_EQ(kinds[3], EditorRenderResultKind::FrameSubmitted);
  EXPECT_EQ(kinds[4], EditorRenderResultKind::FramePresented);
}

TEST_F(EditorRenderCoordinatorTest, RejectsPresentedWithoutSubmittedAndIgnoresDuplicates) {
  const auto accepted = coordinator_->Submit(
      MakeIntent(EditorRenderQuality::Interactive, EditorRenderPriority::Normal));
  coordinator_->NotifySchedulerCompleted(accepted.request_id, true);

  // Presented without submitted must be ignored.
  coordinator_->NotifyFramePresented(accepted.request_id);
  for (const auto& r : coordinator_->results()) {
    EXPECT_NE(r.kind, EditorRenderResultKind::FramePresented);
  }

  coordinator_->NotifyFrameSubmitted(accepted.request_id);
  coordinator_->NotifyFramePresented(accepted.request_id);
  coordinator_->NotifyFrameSubmitted(accepted.request_id);
  coordinator_->NotifyFramePresented(accepted.request_id);

  int submitted = 0;
  int presented = 0;
  for (const auto& r : coordinator_->results()) {
    if (r.request_id != accepted.request_id) {
      continue;
    }
    if (r.kind == EditorRenderResultKind::FrameSubmitted) {
      ++submitted;
    }
    if (r.kind == EditorRenderResultKind::FramePresented) {
      ++presented;
    }
  }
  EXPECT_EQ(submitted, 1);
  EXPECT_EQ(presented, 1);
}

TEST_F(EditorRenderCoordinatorTest, CancelInflightStartsUnrelatedPendingRequest) {
  const auto inflight = coordinator_->Submit(
      MakeIntent(EditorRenderQuality::Interactive, EditorRenderPriority::Normal));
  ASSERT_TRUE(coordinator_->has_inflight());

  auto pending            = MakeIntent(EditorRenderQuality::Quality, EditorRenderPriority::Normal);
  pending.replacement_key = "quality";
  const auto pending_result = coordinator_->Submit(pending);
  EXPECT_EQ(coordinator_->pending_count(), 1u);

  EXPECT_TRUE(coordinator_->CancelRequest(inflight.request_id));
  EXPECT_FALSE(scheduler_->cancelled_.empty());
  // Unrelated pending quality request must start immediately.
  EXPECT_TRUE(coordinator_->has_inflight());
  EXPECT_EQ(coordinator_->last_scheduled_request_id(), pending_result.request_id);
  EXPECT_EQ(coordinator_->pending_count(), 0u);

  // Only one cancel result for the inflight request.
  int cancel_count = 0;
  for (const auto& r : coordinator_->results()) {
    if (r.request_id == inflight.request_id && r.kind == EditorRenderResultKind::Cancelled) {
      ++cancel_count;
    }
  }
  EXPECT_EQ(cancel_count, 1);
  // Late completion after cancel is ignored.
  coordinator_->NotifySchedulerCompleted(inflight.request_id, true);
  for (const auto& r : coordinator_->results()) {
    if (r.request_id == inflight.request_id) {
      EXPECT_NE(r.kind, EditorRenderResultKind::RenderCompleted);
    }
  }
}

TEST_F(EditorRenderCoordinatorTest,
       SetActiveGenerationsCancelsObsoletePendingAndInflightRenderGen) {
  const auto inflight = coordinator_->Submit(
      MakeIntent(EditorRenderQuality::Interactive, EditorRenderPriority::Normal, 1, 1, 1));
  auto pending = MakeIntent(EditorRenderQuality::Quality, EditorRenderPriority::Normal, 1, 1, 1);
  coordinator_->Submit(pending);
  EXPECT_TRUE(coordinator_->has_inflight());
  EXPECT_EQ(coordinator_->pending_count(), 1u);

  // Advance render generation — both obsolete requests must be cancelled.
  coordinator_->SetActiveGenerations(1, 2, 1);
  EXPECT_FALSE(coordinator_->has_inflight());
  EXPECT_EQ(coordinator_->pending_count(), 0u);
  EXPECT_FALSE(scheduler_->cancelled_.empty());

  int cancelled = 0;
  for (const auto& r : coordinator_->results()) {
    if (r.kind == EditorRenderResultKind::Cancelled) {
      ++cancelled;
    }
  }
  EXPECT_GE(cancelled, 2);

  // New request with current render gen is accepted and scheduled.
  const auto next = coordinator_->Submit(
      MakeIntent(EditorRenderQuality::Interactive, EditorRenderPriority::Normal, 1, 2, 1));
  EXPECT_EQ(next.kind, EditorRenderResultKind::RequestAccepted);
  EXPECT_TRUE(coordinator_->has_inflight());
  EXPECT_EQ(inflight.kind, EditorRenderResultKind::RequestAccepted);
}

TEST_F(EditorRenderCoordinatorTest,
       AdjustmentBurstKeepsRunningFastFrameAndSchedulesOnlyLatestPendingValue) {
  const auto first = coordinator_->Submit(
      MakeIntent(EditorRenderQuality::Interactive, EditorRenderPriority::Normal, 1, 1, 1));
  ASSERT_TRUE(coordinator_->has_inflight());

  coordinator_->SetActiveGenerations(
      1, 2, 1, EditorRenderSupersessionPolicy::PreserveInflightFullFrame);
  EXPECT_TRUE(coordinator_->has_inflight());
  EXPECT_TRUE(scheduler_->cancelled_.empty());

  const auto middle = coordinator_->Submit(
      MakeIntent(EditorRenderQuality::Interactive, EditorRenderPriority::Normal, 1, 2, 1));
  ASSERT_EQ(coordinator_->pending_count(), 1u);

  coordinator_->SetActiveGenerations(
      1, 3, 1, EditorRenderSupersessionPolicy::PreserveInflightFullFrame);
  const auto latest = coordinator_->Submit(
      MakeIntent(EditorRenderQuality::Interactive, EditorRenderPriority::Normal, 1, 3, 1));
  ASSERT_EQ(coordinator_->pending_count(), 1u);
  EXPECT_TRUE(scheduler_->cancelled_.empty());

  coordinator_->NotifySchedulerCompleted(first.request_id, true);

  ASSERT_EQ(scheduler_->scheduled_.size(), 2u);
  EXPECT_EQ(scheduler_->scheduled_.back().request_id, latest.request_id);
  EXPECT_NE(scheduler_->scheduled_.back().request_id, middle.request_id);
}

TEST_F(EditorRenderCoordinatorTest, ViewGenerationAdvanceCancelsOnlyDetailPatch) {
  // Phase 5D D2: a view-generation advance (zoom/pan/resize/ROI) only obsoletes
  // view-dependent DetailPatch work. Full-frame renders (InteractivePrimary /
  // QualityBase) are view-independent — the renderer re-samples them under the
  // new view — so a zoom must NOT cancel an in-flight or pending full-frame.
  const auto interactive = coordinator_->Submit(
      MakeIntent(EditorRenderQuality::Interactive, EditorRenderPriority::Normal, 1, 1, 1));
  ASSERT_TRUE(coordinator_->has_inflight());
  const auto detail = coordinator_->Submit(
      MakeIntent(EditorRenderQuality::Detail, EditorRenderPriority::Low, 1, 1, 1));
  ASSERT_EQ(coordinator_->pending_count(), 1u);

  coordinator_->SetActiveGenerations(1, 1, 2);

  // The interactive full-frame in-flight survives; only the DetailPatch pending
  // is cancelled.
  EXPECT_TRUE(coordinator_->has_inflight());
  EXPECT_EQ(coordinator_->pending_count(), 0u);

  bool detail_cancelled      = false;
  bool interactive_cancelled = false;
  for (const auto& r : coordinator_->results()) {
    if (r.kind != EditorRenderResultKind::Cancelled) {
      continue;
    }
    if (r.request_id == detail.request_id) {
      detail_cancelled = true;
    }
    if (r.request_id == interactive.request_id) {
      interactive_cancelled = true;
    }
  }
  EXPECT_TRUE(detail_cancelled);
  EXPECT_FALSE(interactive_cancelled);
}

TEST_F(EditorRenderCoordinatorTest, ViewGenerationAdvanceKeepsFullFramePendingCancelsOnlyDetail) {
  // DetailPatch in-flight; QualityBase + InteractivePrimary pending (both full
  // frame). A view-generation advance cancels the in-flight DetailPatch but
  // keeps both full-frame pending entries so the viewport re-samples them under
  // the new view (Phase 5D D2).
  const auto detail = coordinator_->Submit(
      MakeIntent(EditorRenderQuality::Detail, EditorRenderPriority::Low, 1, 1, 1));
  ASSERT_TRUE(coordinator_->has_inflight());
  const auto quality = coordinator_->Submit(
      MakeIntent(EditorRenderQuality::Quality, EditorRenderPriority::Normal, 1, 1, 1));
  const auto interactive = coordinator_->Submit(
      MakeIntent(EditorRenderQuality::Interactive, EditorRenderPriority::Normal, 1, 1, 1));
  ASSERT_EQ(coordinator_->pending_count(), 2u);

  coordinator_->SetActiveGenerations(1, 1, 2);

  // The in-flight DetailPatch is cancelled; ScheduleNext then promotes the
  // higher-role full-frame (InteractivePrimary, role 3) to in-flight, leaving
  // the QualityBase pending. Both full-frames survive (one in-flight, one
  // pending); neither is cancelled.
  EXPECT_TRUE(coordinator_->has_inflight());     // detail cancelled; full-frame promoted
  EXPECT_EQ(coordinator_->pending_count(), 1u);  // one full-frame pending, one in-flight

  bool detail_cancelled     = false;
  int  full_frame_cancelled = 0;
  for (const auto& r : coordinator_->results()) {
    if (r.kind != EditorRenderResultKind::Cancelled) {
      continue;
    }
    if (r.request_id == detail.request_id) {
      detail_cancelled = true;
    }
    if (r.request_id == quality.request_id || r.request_id == interactive.request_id) {
      ++full_frame_cancelled;
    }
  }
  EXPECT_TRUE(detail_cancelled);
  EXPECT_EQ(full_frame_cancelled, 0);
}

TEST_F(EditorRenderCoordinatorTest, SubmitDoesNotMutateStoredIntentAfterAccept) {
  EditorRenderIntent intent =
      MakeIntent(EditorRenderQuality::Quality, EditorRenderPriority::Normal);
  intent.replacement_key.clear();
  intent.adjustment.fingerprint = "tone:v1";
  intent.adjustment.params_json = R"({"exposure":0.5})";
  intent.adjustment.patches.push_back(EditorAdjustmentPatch{"exposure", R"({"v":0.5})", false});

  const auto accepted = coordinator_->Submit(intent);
  EXPECT_EQ(accepted.kind, EditorRenderResultKind::RequestAccepted);
  EXPECT_EQ(accepted.intent.replacement_key, "quality");
  EXPECT_EQ(accepted.intent.frame_role, FrameRole::QualityBase);
  EXPECT_EQ(accepted.intent.adjustment.fingerprint, "tone:v1");
  ASSERT_EQ(accepted.intent.adjustment.patches.size(), 1u);
  EXPECT_EQ(accepted.intent.adjustment.patches[0].field_key, "exposure");
  EXPECT_EQ(accepted.intent.adjustment.params_json, R"({"exposure":0.5})");

  ASSERT_FALSE(scheduler_->scheduled_.empty());
  const auto& scheduled_intent = scheduler_->scheduled_.front().intent;
  EXPECT_EQ(scheduled_intent.adjustment, accepted.intent.adjustment);
  EXPECT_EQ(scheduled_intent.replacement_key, accepted.intent.replacement_key);
  EXPECT_EQ(scheduled_intent.frame_role, accepted.intent.frame_role);
}

TEST_F(EditorRenderCoordinatorTest, IsTheOnlySchedulerCallerThroughSubmitPort) {
  IEditorRenderSubmitPort* port = coordinator_.get();
  port->SetActiveGenerations(1, 1, 1);
  const auto result =
      port->Submit(MakeIntent(EditorRenderQuality::Interactive, EditorRenderPriority::Normal));
  EXPECT_EQ(result.kind, EditorRenderResultKind::RequestAccepted);
  EXPECT_EQ(scheduler_->scheduled_.size(), 1u);
}

// ---------------------------------------------------------------------------
// Phase 5D per-reason coordinator decisions (D2 reuse-vs-render, A1).

TEST(EditorRenderIntentPolicyTest, ViewDependentReasonsDoNotReplayAdjustmentSnapshot) {
  // Operator params are applied incrementally on content change. Detail ROI /
  // scope ROI / pure view transforms must not re-ApplyEditorAdjustmentSnapshot
  // (that path thrash-invalidates Image Loading / RAW_DECODE).
  EXPECT_FALSE(ReasonAppliesAdjustmentSnapshot(EditorRenderReason::ZoomPan));
  EXPECT_FALSE(ReasonAppliesAdjustmentSnapshot(EditorRenderReason::Resize));
  EXPECT_FALSE(ReasonAppliesAdjustmentSnapshot(EditorRenderReason::DetailRefresh));
  EXPECT_FALSE(ReasonAppliesAdjustmentSnapshot(EditorRenderReason::ScopeRefresh));

  EXPECT_TRUE(ReasonAppliesAdjustmentSnapshot(EditorRenderReason::InitialFrame));
  EXPECT_TRUE(ReasonAppliesAdjustmentSnapshot(EditorRenderReason::InteractiveAdjustment));
  EXPECT_TRUE(ReasonAppliesAdjustmentSnapshot(EditorRenderReason::SettledAdjustment));
  EXPECT_TRUE(ReasonAppliesAdjustmentSnapshot(EditorRenderReason::UndoRedo));
  EXPECT_TRUE(ReasonAppliesAdjustmentSnapshot(EditorRenderReason::ImageSwitch));
  EXPECT_TRUE(ReasonAppliesAdjustmentSnapshot(EditorRenderReason::Retry));
  EXPECT_TRUE(ReasonAppliesAdjustmentSnapshot(EditorRenderReason::CropRotate));
}

TEST_F(EditorRenderCoordinatorTest, ZoomPanIntentIsReusedWithoutScheduling) {
  // A pure zoom/pan transform reuses the current full frame; the renderer
  // re-samples it through synchronize(). No pipeline task is scheduled.
  const auto result = coordinator_->Submit(
      MakeViewIntent(EditorRenderReason::ZoomPan, EditorRenderQuality::Interactive));
  EXPECT_EQ(result.kind, EditorRenderResultKind::Reused);
  EXPECT_TRUE(scheduler_->scheduled_.empty());
  EXPECT_FALSE(coordinator_->has_inflight());
  EXPECT_EQ(coordinator_->pending_count(), 0u);
}

TEST_F(EditorRenderCoordinatorTest, ResizeIntentIsReusedWithoutScheduling) {
  // Phase 5D D8: a viewport resize reuses the current full frame; the render
  // pass rebuilds QRhi objects in initialize() without a new pipeline task and
  // without invalidating active image generation.
  const auto result = coordinator_->Submit(
      MakeViewIntent(EditorRenderReason::Resize, EditorRenderQuality::Interactive));
  EXPECT_EQ(result.kind, EditorRenderResultKind::Reused);
  EXPECT_TRUE(scheduler_->scheduled_.empty());
  EXPECT_FALSE(coordinator_->has_inflight());
  EXPECT_EQ(coordinator_->pending_count(), 0u);
}

TEST_F(EditorRenderCoordinatorTest, DetailRefreshSchedulesDetailPatch) {
  const auto result = coordinator_->Submit(
      MakeViewIntent(EditorRenderReason::DetailRefresh, EditorRenderQuality::Detail));
  EXPECT_EQ(result.kind, EditorRenderResultKind::RequestAccepted);
  ASSERT_EQ(scheduler_->scheduled_.size(), 1u);
  const auto& scheduled = scheduler_->scheduled_.back().intent;
  EXPECT_EQ(scheduled.frame_role, FrameRole::DetailPatch);
  EXPECT_EQ(scheduled.reason, EditorRenderReason::DetailRefresh);
  EXPECT_EQ(scheduled.quality, EditorRenderQuality::Detail);
  EXPECT_TRUE(coordinator_->has_inflight());
}

// Production Cancel fires the request token callback, which re-enters
// CancelRequest. That path must not run under the coordinator mutex or a
// zoom-driven view-generation advance deadlocks and no further DetailPatch
// work (or any pipeline request) is ever scheduled.
TEST_F(EditorRenderCoordinatorTest, ViewGenerationCancelDoesNotDeadlockOnTokenReentry) {
  class ReentrantCancelScheduler final : public IEditorPipelineSchedulerPort {
   public:
    auto Schedule(const EditorRenderRequest& request) -> std::uint64_t override {
      scheduled_.push_back(request);
      tokens_[++next_job_] = request.intent.cancellation;
      return next_job_;
    }
    void Cancel(std::uint64_t job_id) override {
      cancelled_.push_back(job_id);
      auto it = tokens_.find(job_id);
      if (it != tokens_.end() && it->second) {
        // Mirrors EditorSessionRenderSchedulerPort::Cancel: token Cancel
        // invokes the coordinator's CancelRequest re-entry path.
        it->second->Cancel();
      }
    }

    std::vector<EditorRenderRequest>                                                  scheduled_;
    std::vector<std::uint64_t>                                                        cancelled_;
    std::uint64_t                                                                     next_job_ = 0;
    std::unordered_map<std::uint64_t, std::shared_ptr<EditorRenderCancellationToken>> tokens_;
  };

  auto reentrant = std::make_shared<ReentrantCancelScheduler>();
  auto coord     = std::make_unique<EditorRenderCoordinator>(reentrant);
  coord->SetActiveGenerations(1, 1, 1);

  auto first = MakeViewIntent(EditorRenderReason::DetailRefresh, EditorRenderQuality::Detail);
  first.cancellation  = std::make_shared<EditorRenderCancellationToken>();
  const auto accepted = coord->Submit(first);
  ASSERT_EQ(accepted.kind, EditorRenderResultKind::RequestAccepted);
  ASSERT_TRUE(coord->has_inflight());
  // Install the same cancel bridge production uses after ScheduleNext.
  first.cancellation->SetCancelCallback([c = coord.get(), id = accepted.request_id] {
    if (c) {
      c->CancelRequest(id);
    }
  });

  // Zoom/pan advances view generation while a DetailPatch is in-flight. This
  // used to deadlock: CancelObsolete → scheduler Cancel → token callback →
  // CancelRequest under the same mutex.
  coord->SetActiveGenerations(1, 1, 2);

  auto second = MakeViewIntent(EditorRenderReason::DetailRefresh, EditorRenderQuality::Detail,
                               EditorRenderPriority::Normal, 1, 1, 2);
  second.cancellation = std::make_shared<EditorRenderCancellationToken>();
  const auto next     = coord->Submit(second);
  EXPECT_EQ(next.kind, EditorRenderResultKind::RequestAccepted);
  EXPECT_TRUE(coord->has_inflight());
  ASSERT_EQ(reentrant->scheduled_.size(), 2u);
  EXPECT_EQ(reentrant->scheduled_.back().request_id, next.request_id);
  EXPECT_FALSE(reentrant->cancelled_.empty());
}

TEST_F(EditorRenderCoordinatorTest, CropRotateSchedulesInteractivePrimary) {
  // A crop rect / rotation change alters rendered content, so the coordinator
  // schedules a fresh InteractivePrimary instead of reusing the current frame.
  const auto result = coordinator_->Submit(
      MakeViewIntent(EditorRenderReason::CropRotate, EditorRenderQuality::Interactive));
  EXPECT_EQ(result.kind, EditorRenderResultKind::RequestAccepted);
  ASSERT_EQ(scheduler_->scheduled_.size(), 1u);
  const auto& scheduled = scheduler_->scheduled_.back().intent;
  EXPECT_EQ(scheduled.frame_role, FrameRole::InteractivePrimary);
  EXPECT_EQ(scheduled.reason, EditorRenderReason::CropRotate);
  EXPECT_EQ(scheduled.quality, EditorRenderQuality::Interactive);
  EXPECT_TRUE(coordinator_->has_inflight());
}

TEST_F(EditorRenderCoordinatorTest, BurstOfReplaceableIntentsKeepsNewestInteractiveOnly) {
  // Phase 5D A2/D3: a burst of replaceable input (slider/pointer updates sharing
  // a replacement key) does not create one task per event. Prior pending entries
  // are replaced; only the newest interactive state survives to render.
  coordinator_->Submit(MakeIntent(EditorRenderQuality::Interactive, EditorRenderPriority::Normal));
  ASSERT_TRUE(coordinator_->has_inflight());

  std::uint64_t last_pending_id = 0;
  constexpr int  kPreviewBurst  = 100;
  for (int i = 0; i < kPreviewBurst; ++i) {
    auto intent = MakeIntent(EditorRenderQuality::Interactive, EditorRenderPriority::Normal);
    intent.replacement_key = "interactive";  // same key → replace prior pending
    const auto result      = coordinator_->Submit(intent);
    ASSERT_EQ(result.kind, EditorRenderResultKind::RequestAccepted);
    last_pending_id = result.request_id;
  }
  // One hundred preview updates but only one pending entry — the newest — survives.
  EXPECT_EQ(coordinator_->pending_count(), 1u);

  int replaced = 0;
  for (const auto& r : coordinator_->results()) {
    if (r.kind == EditorRenderResultKind::Replaced) {
      ++replaced;
    }
  }
  EXPECT_EQ(replaced, kPreviewBurst - 1);

  // Complete the in-flight first frame; the newest interactive state is scheduled.
  const auto inflight_id = coordinator_->last_scheduled_request_id();
  coordinator_->NotifySchedulerCompleted(inflight_id, true);
  ASSERT_EQ(scheduler_->scheduled_.size(), 2u);
  EXPECT_EQ(scheduler_->scheduled_.back().request_id, last_pending_id);
}

TEST_F(EditorRenderCoordinatorTest, InteractiveNotBlockedBehindOutdatedDetail) {
  // Phase 5D A3/D4: interactive work is not blocked behind an outdated detail
  // patch. With Quality in-flight and Interactive + Detail pending, completing
  // the in-flight work schedules Interactive (role 3) before Detail (role 1),
  // even though Detail carries the higher EditorRenderPriority.
  coordinator_->Submit(MakeIntent(EditorRenderQuality::Quality, EditorRenderPriority::Normal));
  ASSERT_TRUE(coordinator_->has_inflight());
  const auto inflight_id = coordinator_->last_scheduled_request_id();

  const auto interactive = coordinator_->Submit(
      MakeIntent(EditorRenderQuality::Interactive, EditorRenderPriority::Normal));
  const auto detail =
      coordinator_->Submit(MakeIntent(EditorRenderQuality::Detail, EditorRenderPriority::High));
  ASSERT_EQ(coordinator_->pending_count(), 2u);

  coordinator_->NotifySchedulerCompleted(inflight_id, true);
  ASSERT_EQ(scheduler_->scheduled_.size(), 2u);
  EXPECT_EQ(scheduler_->scheduled_.back().request_id, interactive.request_id);
  EXPECT_NE(scheduler_->scheduled_.back().request_id, detail.request_id);
}

TEST_F(EditorRenderCoordinatorTest, DiagnosticsTrackRejectReplaceCancelAndSubmit) {
  // Phase 5E: diagnostics must explain why work was requested, replaced,
  // cancelled, presented, or rejected without exposing pipeline task objects.
  auto rejected = coordinator_->Submit(
      MakeIntent(EditorRenderQuality::Interactive, EditorRenderPriority::Normal, /*session=*/9));
  EXPECT_EQ(rejected.kind, EditorRenderResultKind::Failed);

  auto first = coordinator_->Submit(
      MakeIntent(EditorRenderQuality::Interactive, EditorRenderPriority::Normal));
  ASSERT_EQ(first.kind, EditorRenderResultKind::RequestAccepted);
  auto older =
      coordinator_->Submit(MakeIntent(EditorRenderQuality::Interactive, EditorRenderPriority::Low));
  auto newer = coordinator_->Submit(
      MakeIntent(EditorRenderQuality::Interactive, EditorRenderPriority::High));
  EXPECT_EQ(older.kind, EditorRenderResultKind::RequestAccepted);
  EXPECT_EQ(newer.kind, EditorRenderResultKind::RequestAccepted);

  {
    const auto diag = coordinator_->diagnostics();
    EXPECT_TRUE(diag.has_inflight);
    EXPECT_EQ(diag.pending_count, 1u);
    EXPECT_GE(diag.replaced_count, 1u);
    EXPECT_EQ(diag.image_load_request_id, 1u);
    EXPECT_FALSE(diag.last_rejection_reason.empty());
    EXPECT_NE(diag.last_rejection_reason.find("image load request"), std::string::npos);
    ASSERT_TRUE(diag.last_rejected_render_reason.has_value());
    EXPECT_EQ(*diag.last_rejected_render_reason, EditorRenderReason::InteractiveAdjustment);
    EXPECT_GE(diag.accepted_count, 2u);
    EXPECT_GE(diag.failed_count, 1u);
  }

  coordinator_->NotifySchedulerCompleted(first.request_id, true);
  coordinator_->NotifyFrameSubmitted(first.request_id);
  {
    const auto diag = coordinator_->diagnostics();
    ASSERT_TRUE(diag.last_submitted_frame_role.has_value());
    EXPECT_EQ(*diag.last_submitted_frame_role, FrameRole::InteractivePrimary);
    ASSERT_TRUE(diag.last_submitted_render_reason.has_value());
    EXPECT_EQ(*diag.last_submitted_render_reason, EditorRenderReason::InteractiveAdjustment);
  }

  // Cancel remaining pending/in-flight work from the replacement burst.
  coordinator_->CancelSession(1);
  {
    const auto diag = coordinator_->diagnostics();
    EXPECT_FALSE(diag.has_inflight);
    EXPECT_EQ(diag.pending_count, 0u);
    EXPECT_GE(diag.cancelled_count, 1u);
  }
}

}  // namespace
}  // namespace alcedo
