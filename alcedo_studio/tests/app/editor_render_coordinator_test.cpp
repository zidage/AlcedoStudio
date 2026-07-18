//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_render_coordinator.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
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

  std::vector<EditorRenderRequest> scheduled_;
  std::vector<std::uint64_t>       cancelled_;
  std::uint64_t                    next_job_  = 0;
  bool                             fail_next_ = false;
};

auto MakeIntent(EditorRenderQuality quality, EditorRenderPriority priority,
                std::uint64_t session_gen = 1, std::uint64_t render_gen = 1,
                std::uint64_t view_gen = 1) -> EditorRenderIntent {
  EditorRenderIntent intent;
  intent.element_id         = 10;
  intent.image_id           = 20;
  intent.session_generation = session_gen;
  intent.render_generation  = render_gen;
  intent.view_generation    = view_gen;
  intent.quality            = quality;
  intent.priority           = priority;
  intent.frame_role         = FrameRoleForQuality(quality);
  intent.replacement_key    = DefaultReplacementKey(quality);
  intent.reason             = EditorRenderReason::InteractiveAdjustment;
  return intent;
}

class EditorRenderCoordinatorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    scheduler_ = std::make_shared<RecordingScheduler>();
    coordinator_ = std::make_unique<EditorRenderCoordinator>(scheduler_);
    coordinator_->SetActiveGenerations(1, 1, 1);
  }

  std::shared_ptr<RecordingScheduler>      scheduler_;
  std::unique_ptr<EditorRenderCoordinator> coordinator_;
};

TEST_F(EditorRenderCoordinatorTest, AcceptsIntentAndSchedulesThroughSingleOwner) {
  const auto accepted = coordinator_->Submit(MakeIntent(EditorRenderQuality::Interactive,
                                                        EditorRenderPriority::Normal));
  EXPECT_EQ(accepted.kind, EditorRenderResultKind::RequestAccepted);
  ASSERT_EQ(scheduler_->scheduled_.size(), 1u);
  EXPECT_EQ(scheduler_->scheduled_.front().request_id, accepted.request_id);
  EXPECT_TRUE(coordinator_->has_inflight());
}

TEST_F(EditorRenderCoordinatorTest, RejectsStaleSessionGeneration) {
  const auto rejected =
      coordinator_->Submit(MakeIntent(EditorRenderQuality::Interactive,
                                      EditorRenderPriority::Normal, /*session=*/2));
  EXPECT_EQ(rejected.kind, EditorRenderResultKind::Failed);
  EXPECT_TRUE(scheduler_->scheduled_.empty());
  EXPECT_NE(rejected.message.find("session"), std::string::npos);
}

TEST_F(EditorRenderCoordinatorTest, ReplacesPendingWorkWithSameReplacementKey) {
  auto first = coordinator_->Submit(
      MakeIntent(EditorRenderQuality::Interactive, EditorRenderPriority::Normal));
  ASSERT_TRUE(coordinator_->has_inflight());

  auto older = MakeIntent(EditorRenderQuality::Interactive, EditorRenderPriority::Low);
  older.reason = EditorRenderReason::ZoomPan;
  const auto pending_old = coordinator_->Submit(older);
  EXPECT_EQ(pending_old.kind, EditorRenderResultKind::RequestAccepted);
  EXPECT_EQ(coordinator_->pending_count(), 1u);

  auto newer = MakeIntent(EditorRenderQuality::Interactive, EditorRenderPriority::High);
  newer.reason = EditorRenderReason::ZoomPan;
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

TEST_F(EditorRenderCoordinatorTest, SchedulesQualityBeforeInteractiveWhenBothPending) {
  coordinator_->Submit(MakeIntent(EditorRenderQuality::Detail, EditorRenderPriority::Low));
  ASSERT_TRUE(coordinator_->has_inflight());
  const auto inflight_id = coordinator_->last_scheduled_request_id();

  const auto interactive = coordinator_->Submit(
      MakeIntent(EditorRenderQuality::Interactive, EditorRenderPriority::High));
  const auto quality = coordinator_->Submit(
      MakeIntent(EditorRenderQuality::Quality, EditorRenderPriority::Low));
  EXPECT_EQ(coordinator_->pending_count(), 2u);

  coordinator_->NotifySchedulerCompleted(inflight_id, true);
  ASSERT_EQ(scheduler_->scheduled_.size(), 2u);
  EXPECT_EQ(scheduler_->scheduled_.back().request_id, quality.request_id);
  EXPECT_NE(scheduler_->scheduled_.back().request_id, interactive.request_id);
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

TEST_F(EditorRenderCoordinatorTest, CancellationTokenPreventsSchedule) {
  coordinator_->Submit(
      MakeIntent(EditorRenderQuality::Interactive, EditorRenderPriority::Normal));
  auto intent = MakeIntent(EditorRenderQuality::Quality, EditorRenderPriority::High);
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

  auto pending = MakeIntent(EditorRenderQuality::Quality, EditorRenderPriority::Normal);
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

TEST_F(EditorRenderCoordinatorTest, SetActiveGenerationsCancelsObsoletePendingAndInflightRenderGen) {
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

TEST_F(EditorRenderCoordinatorTest, SetActiveGenerationsCancelsObsoleteViewGeneration) {
  coordinator_->Submit(
      MakeIntent(EditorRenderQuality::Interactive, EditorRenderPriority::Normal, 1, 1, 1));
  auto pending = MakeIntent(EditorRenderQuality::Detail, EditorRenderPriority::Low, 1, 1, 1);
  coordinator_->Submit(pending);
  EXPECT_EQ(coordinator_->pending_count(), 1u);

  coordinator_->SetActiveGenerations(1, 1, 2);
  EXPECT_FALSE(coordinator_->has_inflight());
  EXPECT_EQ(coordinator_->pending_count(), 0u);

  const auto next = coordinator_->Submit(
      MakeIntent(EditorRenderQuality::Interactive, EditorRenderPriority::Normal, 1, 1, 2));
  EXPECT_EQ(next.kind, EditorRenderResultKind::RequestAccepted);
  EXPECT_TRUE(coordinator_->has_inflight());
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

}  // namespace
}  // namespace alcedo
