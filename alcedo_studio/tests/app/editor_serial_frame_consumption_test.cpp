//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_monotonic_clock.hpp"
#include "app/editor_render_coordinator.hpp"
#include "app/editor_session_bootstrap.hpp"
#include "app/editor_session_service.hpp"
#include "support/editor_session_command_queue_test_support.hpp"
#include "support/editor_parameter_write_test.hpp"
#include "support/latch_blocked_pipeline_scheduler_port.hpp"
#include "support/manual_monotonic_clock.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace alcedo {
namespace {

class ManualEditorClock final : public IEditorMonotonicClock {
 public:
  test::ManualMonotonicClock clock;

  [[nodiscard]] auto NowNs() const -> std::int64_t override { return clock.now_ns(); }
};

class SerialFrameConsumptionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    history_          = std::make_shared<test::FakeEditorHistoryPort>();
    pipeline_         = std::make_shared<test::FakeEditorPipelinePort>();
    tasks_            = std::make_shared<test::FakeEditorTaskPort>();
    journal_          = std::make_shared<test::FakeEditorJournalPort>();
    checkpoint_store_ = std::make_shared<test::FakeEditorCheckpointStore>();
    latch_            = std::make_shared<test::LatchBlockedPipelineSchedulerPort>();
    clock_            = std::make_shared<ManualEditorClock>();
    runtime_          = EditorSessionRuntime::CreateWithPorts(
        pipeline_, history_, tasks_, journal_, latch_, checkpoint_store_);
    service_ = runtime_->service.get();
    service_->SetMonotonicClock(clock_);
    service_->SetAdmissionDeadlineHandler(
        [this](std::int64_t delay_ns) { last_deadline_delay_ns_ = delay_ns; });
    service_->SetPresentationSinkId(1);
    service_->SetPresentationSize(640, 480);
  }

  void CompleteInflightAndDrain() {
    for (int i = 0; i < 16; ++i) {
      service_->DrainCommandQueueForTests();
      if (latch_->running()) {
        latch_->Complete(true);
        continue;
      }
      if (!runtime_->coordinator->has_inflight() &&
          !service_->serial_frame_admission().HoldsOwnership()) {
        return;
      }
    }
  }

  void OpenInteractive() {
    (void)service_->Open(10, 20);
    CompleteInflightAndDrain();
    ASSERT_EQ(service_->state(), EditorSessionState::Interactive);
    ASSERT_FALSE(latch_->running());
    ASSERT_FALSE(runtime_->coordinator->has_inflight());
  }

  void EnqueueExposure(float value, bool settled = false) {
    EditorAdjustmentPatch patch = test::ScalarPatch("exposure", value, settled);
    ASSERT_EQ(service_->EnqueueAdjustmentInput(patch).kind, EditorSessionResultKind::Accepted);
  }

  void ConsumeQueued() {
    service_->DrainCommandQueueForTests();
    if (!runtime_->coordinator->has_inflight() &&
        !service_->serial_frame_admission().HoldsOwnership()) {
      service_->TryConsumePendingInput();
    }
  }

  std::shared_ptr<test::FakeEditorHistoryPort>             history_;
  std::shared_ptr<test::FakeEditorPipelinePort>            pipeline_;
  std::shared_ptr<test::FakeEditorTaskPort>                tasks_;
  std::shared_ptr<test::FakeEditorJournalPort>             journal_;
  std::shared_ptr<test::FakeEditorCheckpointStore>         checkpoint_store_;
  std::shared_ptr<test::LatchBlockedPipelineSchedulerPort> latch_;
  std::shared_ptr<ManualEditorClock>                       clock_;
  std::unique_ptr<EditorSessionRuntime>                    runtime_;
  EditorSessionService*                                    service_                = nullptr;
  std::int64_t                                             last_deadline_delay_ns_ = 0;
};

TEST_F(SerialFrameConsumptionTest, ReleaseBeforeFirstPreviewCommitsFinalValuesOnce) {
  OpenInteractive();
  const int captures_before = history_->capture_count;
  EnqueueExposure(1.25f, true);
  ConsumeQueued();
  ASSERT_TRUE(latch_->running());
  EXPECT_EQ(history_->capture_count, captures_before + 1);
  EXPECT_EQ(history_->commit_count, 1);
  ASSERT_TRUE(history_->last_committed_patch.write.has_value());
  EXPECT_EQ(alcedo::test::ScalarValue(*history_->last_committed_patch.write), 1.25f);
  ASSERT_FALSE(latch_->scheduled().empty());
  EXPECT_EQ(latch_->scheduled().back().intent.quality, EditorRenderQuality::Quality);
  EXPECT_TRUE(latch_->scheduled().back().intent.live_parameters_applied);
  EXPECT_TRUE(service_->PeekPendingInput().sequences.empty());
}

TEST_F(SerialFrameConsumptionTest, InteractiveCycleUsesRemainingTimeWithinSixteenMilliseconds) {
  OpenInteractive();
  const auto scheduled_open = latch_->scheduled().size();
  clock_->clock.set_ns(0);
  EnqueueExposure(0.10f);
  ConsumeQueued();
  ASSERT_TRUE(latch_->running());
  EXPECT_EQ(latch_->scheduled().size(), scheduled_open + 1);
  ASSERT_EQ(service_->serial_frame_admission().interactive_start_times_ns().size(), 1u);
  EXPECT_EQ(service_->serial_frame_admission().interactive_start_times_ns().front(), 0);

  EnqueueExposure(0.20f);
  service_->DrainCommandQueueForTests();
  EXPECT_EQ(latch_->scheduled().size(), scheduled_open + 1);
  EXPECT_EQ(latch_->rejected_while_running(), 0);
  EXPECT_TRUE(service_->serial_frame_admission().HoldsOwnership());

  clock_->clock.set_ns(5'000'000);
  latch_->Complete(true);
  service_->DrainCommandQueueForTests();
  EXPECT_FALSE(runtime_->coordinator->has_inflight());
  EXPECT_EQ(last_deadline_delay_ns_, 11'000'000);
  EXPECT_EQ(service_->serial_frame_admission().interactive_complete_times_ns().size(), 1u);
  EXPECT_EQ(latch_->scheduled().size(), scheduled_open + 1);

  clock_->clock.set_ns(16'000'000);
  service_->TryConsumePendingInput();
  ASSERT_TRUE(latch_->running());
  ASSERT_EQ(service_->serial_frame_admission().interactive_start_times_ns().size(), 2u);
  EXPECT_EQ(service_->serial_frame_admission().interactive_start_times_ns().back(), 16'000'000);
}

TEST_F(SerialFrameConsumptionTest, OverBudgetInteractiveCycleStartsNextOnlyAfterCompletion) {
  OpenInteractive();
  clock_->clock.set_ns(0);
  EnqueueExposure(0.10f);
  ConsumeQueued();
  ASSERT_TRUE(latch_->running());
  EnqueueExposure(0.30f);
  clock_->clock.set_ns(22'000'000);
  latch_->Complete(true);
  service_->DrainCommandQueueForTests();
  ASSERT_TRUE(latch_->running());
  ASSERT_EQ(service_->serial_frame_admission().interactive_start_times_ns().size(), 2u);
  EXPECT_EQ(service_->serial_frame_admission().interactive_start_times_ns().back(), 22'000'000);
}

TEST_F(SerialFrameConsumptionTest, QualityReleaseBypassesPacingAfterCurrentFrameCompletes) {
  OpenInteractive();
  clock_->clock.set_ns(0);
  EnqueueExposure(0.10f);
  ConsumeQueued();
  ASSERT_TRUE(latch_->running());
  EnqueueExposure(0.40f, true);
  clock_->clock.set_ns(5'000'000);
  latch_->Complete(true);
  service_->DrainCommandQueueForTests();
  ASSERT_TRUE(latch_->running());
  EXPECT_EQ(latch_->scheduled().back().intent.quality, EditorRenderQuality::Quality);
  EXPECT_EQ(history_->commit_count, 1);
  ASSERT_EQ(service_->serial_frame_admission().interactive_start_times_ns().size(), 1u);
}

TEST_F(SerialFrameConsumptionTest, FailedOrCancelledRenderReleasesOwnerWithoutPublishingResult) {
  OpenInteractive();
  EnqueueExposure(0.15f);
  ConsumeQueued();
  ASSERT_TRUE(service_->serial_frame_admission().HoldsOwnership());
  const auto ready_before = runtime_->coordinator->diagnostics().ready_count;
  latch_->Complete(false, "pipeline failed");
  service_->DrainCommandQueueForTests();
  EXPECT_FALSE(service_->serial_frame_admission().HoldsOwnership());
  EXPECT_EQ(runtime_->coordinator->diagnostics().ready_count, ready_before);
  EXPECT_GT(runtime_->coordinator->diagnostics().failed_count, 0u);
  EXPECT_FALSE(runtime_->coordinator->has_inflight());

  EnqueueExposure(0.16f);
  ConsumeQueued();
  EXPECT_TRUE(service_->serial_frame_admission().HoldsOwnership());
  EXPECT_TRUE(latch_->running());
}

TEST_F(SerialFrameConsumptionTest, GuiRemainsResponsiveWhilePresentNeedsAnUpdate) {
  OpenInteractive();
  EnqueueExposure(0.11f);
  ConsumeQueued();
  ASSERT_TRUE(latch_->running());
  EditorAdjustmentPatch follow_up;
  follow_up.field_key   = "exposure";
  follow_up.write = alcedo::EditorScalarWrite{0.12f};
  const auto queued     = service_->EnqueueAdjustmentInput(follow_up);
  EXPECT_EQ(queued.kind, EditorSessionResultKind::Accepted);
  EXPECT_TRUE(latch_->running());
  EXPECT_TRUE(service_->serial_frame_admission().HoldsOwnership());
  const auto pending = service_->PeekPendingInput();
  ASSERT_EQ(pending.sequences.size(), 1u);
  ASSERT_FALSE(pending.sequences.front().fields.empty());
  EXPECT_EQ(alcedo::PendingScalarValue(pending.sequences.front().fields.front()), 0.12f);
}

TEST_F(SerialFrameConsumptionTest, UndoAndCheckoutWaitForOwnerWithoutBlockingGui) {
  OpenInteractive();
  EnqueueExposure(0.21f);
  ConsumeQueued();
  ASSERT_TRUE(service_->serial_frame_admission().HoldsOwnership());
  const int  undos_before = history_->undo_count;
  const auto undo         = service_->Undo();
  EXPECT_EQ(undo.kind, EditorSessionResultKind::Accepted);
  EXPECT_NE(undo.message.find("queued"), std::string::npos);
  EXPECT_EQ(history_->undo_count, undos_before);
  EXPECT_TRUE(latch_->running());

  latch_->Complete(true);
  CompleteInflightAndDrain();
  EXPECT_EQ(history_->undo_count, undos_before + 1);
  EXPECT_FALSE(service_->serial_frame_admission().HasDeferredOwnerWork());

  EnqueueExposure(0.22f);
  ConsumeQueued();
  ASSERT_TRUE(service_->serial_frame_admission().HoldsOwnership());
  const int  checkouts_before = history_->checkout_count;
  const auto checkout         = service_->CheckoutVersion(history_->last_root_version);
  EXPECT_EQ(checkout.kind, EditorSessionResultKind::Accepted);
  EXPECT_NE(checkout.message.find("queued"), std::string::npos);
  EXPECT_EQ(history_->checkout_count, checkouts_before);
  EXPECT_TRUE(latch_->running());
  latch_->Complete(true);
  CompleteInflightAndDrain();
  EXPECT_FALSE(service_->serial_frame_admission().HasDeferredOwnerWork());
}

TEST_F(SerialFrameConsumptionTest, DeadlineWithEmptyQueueDoesNotScheduleAnEmptyFrame) {
  OpenInteractive();
  const auto scheduled_before = latch_->scheduled().size();
  clock_->clock.set_ns(16'000'000);
  service_->TryConsumePendingInput();
  EXPECT_EQ(latch_->scheduled().size(), scheduled_before);
  EXPECT_FALSE(runtime_->coordinator->has_inflight());
  EXPECT_FALSE(service_->serial_frame_admission().HoldsOwnership());
}

TEST_F(SerialFrameConsumptionTest, HiddenViewportAbortsCycleWithoutStrandingOwnership) {
  OpenInteractive();
  service_->SetPresentationSinkId(0);
  EnqueueExposure(0.33f);
  ConsumeQueued();
  EXPECT_FALSE(service_->serial_frame_admission().HoldsOwnership());
  EXPECT_FALSE(runtime_->coordinator->has_inflight());
}

}  // namespace
}  // namespace alcedo
