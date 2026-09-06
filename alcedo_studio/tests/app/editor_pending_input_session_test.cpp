//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_pending_input.hpp"
#include "app/editor_render_coordinator.hpp"
#include "app/editor_session_bootstrap.hpp"
#include "app/editor_session_service.hpp"
#include "support/editor_session_command_queue_test_support.hpp"
#include "support/editor_parameter_write_test.hpp"

#include <gtest/gtest.h>

#include <memory>

namespace alcedo {
namespace {

class RecordingScheduler final : public IEditorPipelineSchedulerPort {
 public:
  auto Schedule(const EditorRenderRequest&, EditorPipelineScheduleCompletion = {})
      -> std::uint64_t override {
    return ++next_job_;
  }
  void Cancel(std::uint64_t) override {}
  void WaitForSessionIdle(std::uint64_t) override {}

 private:
  std::uint64_t next_job_ = 0;
};

class EditorPendingInputSessionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    history_          = std::make_shared<test::FakeEditorHistoryPort>();
    pipeline_         = std::make_shared<test::FakeEditorPipelinePort>();
    tasks_            = std::make_shared<test::FakeEditorTaskPort>();
    journal_          = std::make_shared<test::FakeEditorJournalPort>();
    scheduler_        = std::make_shared<RecordingScheduler>();
    checkpoint_store_ = std::make_shared<test::FakeEditorCheckpointStore>();
    runtime_          = EditorSessionRuntime::CreateWithPorts(
        pipeline_, history_, tasks_, journal_, scheduler_, checkpoint_store_);
    service_ = runtime_->service.get();
    service_->SetPresentationSinkId(1);
    service_->SetPresentationSize(640, 480);
  }

  void OpenInteractive() {
    (void)service_->Open(10, 20);
    service_->DrainCommandQueueForTests();
    const auto rid = service_->first_frame_request_id();
    if (rid != 0) {
      runtime_->coordinator->NotifySchedulerCompleted(rid, true);
      service_->DrainCommandQueueForTests();
      const auto quality_rid = runtime_->coordinator->last_scheduled_request_id();
      if (quality_rid != rid) {
        runtime_->coordinator->NotifySchedulerCompleted(quality_rid, true);
        service_->DrainCommandQueueForTests();
      }
    }
    ASSERT_EQ(service_->state(), EditorSessionState::Interactive);
  }

  std::shared_ptr<test::FakeEditorHistoryPort>      history_;
  std::shared_ptr<test::FakeEditorPipelinePort>     pipeline_;
  std::shared_ptr<test::FakeEditorTaskPort>         tasks_;
  std::shared_ptr<test::FakeEditorJournalPort>      journal_;
  std::shared_ptr<RecordingScheduler>               scheduler_;
  std::shared_ptr<test::FakeEditorCheckpointStore>  checkpoint_store_;
  std::unique_ptr<EditorSessionRuntime>             runtime_;
  EditorSessionService*                             service_ = nullptr;
};

TEST_F(EditorPendingInputSessionTest, EnqueueDoesNotCaptureHistoryOrApplyLivePatch) {
  OpenInteractive();
  const int captures_before = history_->capture_count;
  const int commits_before  = history_->commit_count;

  EditorAdjustmentPatch preview = test::ScalarPatch("exposure", 0.25f, false);
  const auto queued   = service_->EnqueueAdjustmentInput(preview);
  EXPECT_EQ(queued.kind, EditorSessionResultKind::Accepted);

  EditorAdjustmentPatch settled = preview;
  settled.write                 = EditorScalarWrite{0.40f};
  settled.settled               = true;
  const auto released           = service_->EnqueueAdjustmentInput(settled);
  EXPECT_EQ(released.kind, EditorSessionResultKind::Accepted);

  EXPECT_EQ(history_->capture_count, captures_before);
  EXPECT_EQ(history_->commit_count, commits_before);
  EXPECT_TRUE(history_->current_snapshot.params_json.empty());
  EXPECT_TRUE(history_->current_snapshot.patches.empty());

  const auto pending = service_->PeekPendingInput();
  ASSERT_EQ(pending.sequences.size(), 1u);
  EXPECT_EQ(pending.sequences.front().seal, EditorPendingInputBoundaryKind::Release);
  const auto* exposure = FindPendingField(pending, "exposure");
  ASSERT_NE(exposure, nullptr);
  EXPECT_EQ(PendingScalarValue(*exposure), 0.40f);
  EXPECT_EQ(exposure->identity.element_id, static_cast<sl_element_id_t>(10));
  EXPECT_EQ(exposure->identity.image_id, static_cast<image_id_t>(20));
}

TEST_F(EditorPendingInputSessionTest, EnqueueRejectedWhenSessionIsNotInteractive) {
  EditorAdjustmentPatch patch = test::ScalarPatch("exposure", 0.25f);
  const auto rejected = service_->EnqueueAdjustmentInput(patch);
  EXPECT_EQ(rejected.kind, EditorSessionResultKind::Rejected);
  EXPECT_TRUE(service_->PeekPendingInput().sequences.empty());
}

TEST_F(EditorPendingInputSessionTest, NodeSwitchBoundaryKeepsOriginalSequenceTarget) {
  OpenInteractive();
  const int captures_before = history_->capture_count;
  EditorAdjustmentPatch first = test::ScalarPatch("exposure", 0.1f);
  first.target.owner_kind             = EditorParameterOwnerKind::ColorGrade;
  first.target.node_id                = NodeId{"grade.a"};
  first.target.adjustment_instance_id = AdjustmentInstanceId{"tone"};
  first.target.field_key              = "exposure";
  ASSERT_EQ(service_->EnqueueAdjustmentInput(first).kind, EditorSessionResultKind::Accepted);
  ASSERT_EQ(service_->EnqueuePendingInputBoundary(EditorPendingInputBoundaryKind::NodeSwitch).kind,
            EditorSessionResultKind::Accepted);

  EditorAdjustmentPatch second = first;
  second.write                 = EditorScalarWrite{0.2f};
  second.target.node_id        = NodeId{"grade.b"};
  ASSERT_EQ(service_->EnqueueAdjustmentInput(second).kind, EditorSessionResultKind::Accepted);

  const auto pending = service_->PeekPendingInput();
  ASSERT_EQ(pending.sequences.size(), 2u);
  EXPECT_EQ(pending.sequences[0].captured_target.node_id, NodeId{"grade.a"});
  EXPECT_EQ(pending.sequences[1].captured_target.node_id, NodeId{"grade.b"});
  EXPECT_EQ(history_->capture_count, captures_before);
}

}  // namespace
}  // namespace alcedo
