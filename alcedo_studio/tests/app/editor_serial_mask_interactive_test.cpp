//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_mask_creation_controller.hpp"
#include "app/editor_monotonic_clock.hpp"
#include "app/editor_render_coordinator.hpp"
#include "app/editor_render_intent.hpp"
#include "app/editor_session_bootstrap.hpp"
#include "app/editor_session_ports.hpp"
#include "app/editor_session_service.hpp"
#include "edit/geometry/types.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "edit/history/pipeline_edit_batch.hpp"
#include "edit/mask/analytic_mask_edit.hpp"
#include "edit/mask/brush_placement.hpp"
#include "edit/mask/mask_id.hpp"
#include "grade_owned_mask_support.hpp"
#include "support/editor_session_command_queue_test_support.hpp"
#include "support/latch_blocked_pipeline_scheduler_port.hpp"
#include "support/manual_monotonic_clock.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace alcedo {
namespace {

const MaskId kMask{"mask.brush"};
constexpr MaskPointerIdentity kPointer{1, 0, 1};

class ManualEditorClock final : public IEditorMonotonicClock {
 public:
  test::ManualMonotonicClock clock;

  [[nodiscard]] auto NowNs() const -> std::int64_t override { return clock.now_ns(); }
};

class DocumentBackedHistoryPort final : public test::FakeEditorHistoryPort {
 public:
  DocumentBackedHistoryPort()
      : graph_(std::make_shared<CommitGraph>(CommitGraph::CreateEmpty(17))),
        journal_(std::make_shared<MiniGitJournal>()),
        history_(graph_, journal_) {
    document = CreateDefaultPipelineDocument();
    grade_mask_test::AddParameterizedBrushMask(
        document, kMask, {grade_mask_test::MakePaintStroke("stroke.1", 8.0f, 6.0f, 3.0f)});
  }

  auto WithLockedLiveDocument(const EditorHistoryGuardHandle&, const LockedMaskDocumentOp& op,
                              std::string* error) -> bool override {
    if (!op) {
      if (error != nullptr) {
        *error = "Locked Mask document operation is empty";
      }
      return false;
    }
    const LockedMaskSettle settle = [](const PipelineEditBatch&, std::string*) { return true; };
    return op(document, history_, settle, error);
  }

  PipelineDocument document;

 private:
  std::shared_ptr<CommitGraph>      graph_;
  std::shared_ptr<MiniGitJournal>   journal_;
  MiniGitWorkingHistory             history_;
};

class SerialMaskInteractiveTest : public ::testing::Test {
 protected:
  void SetUp() override {
    history_          = std::make_shared<DocumentBackedHistoryPort>();
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

  void ConsumeQueued() {
    service_->DrainCommandQueueForTests();
    if (!runtime_->coordinator->has_inflight() &&
        !service_->serial_frame_admission().HoldsOwnership()) {
      service_->TryConsumePendingInput();
    }
  }

  [[nodiscard]] auto GradeId() const -> NodeId {
    return history_->document.PrimaryGrade()->Id();
  }

  [[nodiscard]] auto LiveTranslation() const -> Vector2 {
    return history_->document.PrimaryGrade()->BrushPlacementTranslation(kMask);
  }

  [[nodiscard]] auto SampleAt(Vector2 reference) const -> MaskCreationSample {
    MaskCreationSample sample;
    sample.normalized        = {reference.x / 16.0f, reference.y / 12.0f};
    sample.reference_pixels  = reference;
    sample.inside_photograph = true;
    return sample;
  }

  auto Enqueue(EditorMaskCreationCommand command) -> EditorSessionResult {
    const auto result = service_->EnqueueMaskCreation(std::move(command));
    EXPECT_EQ(result.kind, EditorSessionResultKind::Accepted);
    return result;
  }

  void EnqueueSelectAndArmMove() {
    EditorMaskCreationCommand select;
    select.kind    = EditorMaskCreationCommandKind::SelectMask;
    select.node_id = GradeId();
    select.mask_id = kMask;
    Enqueue(select);

    EditorMaskCreationCommand tool;
    tool.kind       = EditorMaskCreationCommandKind::SetBrushTool;
    tool.brush_tool = EditorBrushTool::Move;
    Enqueue(tool);

    EditorMaskCreationCommand begin;
    begin.kind       = EditorMaskCreationCommandKind::BeginMove;
    begin.node_id    = GradeId();
    begin.mask_id    = kMask;
    begin.handle     = AnalyticMaskHandle::BrushMove;
    begin.brush_tool = EditorBrushTool::Move;
    begin.identity   = kPointer;
    begin.sample     = SampleAt({8.0f, 6.0f});
    Enqueue(begin);
  }

  void EnqueueAppend(Vector2 reference) {
    EditorMaskCreationCommand append;
    append.kind     = EditorMaskCreationCommandKind::Append;
    append.node_id  = GradeId();
    append.mask_id  = kMask;
    append.identity = kPointer;
    append.sample   = SampleAt(reference);
    Enqueue(append);
  }

  [[nodiscard]] auto PendingAppendCount() const -> std::size_t {
    const auto pending = service_->PeekPendingMaskCommands();
    return static_cast<std::size_t>(
        std::count_if(pending.begin(), pending.end(), [](const EditorMaskCreationCommand& command) {
          return command.kind == EditorMaskCreationCommandKind::Append;
        }));
  }

  [[nodiscard]] auto LastPendingAppend() const -> std::optional<EditorMaskCreationCommand> {
    const auto pending = service_->PeekPendingMaskCommands();
    for (auto it = pending.rbegin(); it != pending.rend(); ++it) {
      if (it->kind == EditorMaskCreationCommandKind::Append) {
        return *it;
      }
    }
    return std::nullopt;
  }

  std::shared_ptr<DocumentBackedHistoryPort>                   history_;
  std::shared_ptr<test::FakeEditorPipelinePort>                pipeline_;
  std::shared_ptr<test::FakeEditorTaskPort>                    tasks_;
  std::shared_ptr<test::FakeEditorJournalPort>                 journal_;
  std::shared_ptr<test::FakeEditorCheckpointStore>             checkpoint_store_;
  std::shared_ptr<test::LatchBlockedPipelineSchedulerPort>     latch_;
  std::shared_ptr<ManualEditorClock>                           clock_;
  std::unique_ptr<EditorSessionRuntime>                        runtime_;
  EditorSessionService*                                        service_ = nullptr;
};

TEST_F(SerialMaskInteractiveTest, MaskMoveNeverMutatesSourceDuringRender) {
  OpenInteractive();
  clock_->clock.set_ns(0);
  EnqueueSelectAndArmMove();
  EnqueueAppend({12.0f, 6.0f});
  ConsumeQueued();
  ASSERT_TRUE(latch_->running());
  ASSERT_TRUE(service_->serial_frame_admission().HoldsOwnership());
  const auto first = BrushPlacementForReferenceDrag({0.0f, 0.0f}, {8.0f, 6.0f}, {12.0f, 6.0f});
  EXPECT_EQ(LiveTranslation(), first);

  EnqueueAppend({16.0f, 6.0f});
  EnqueueAppend({20.0f, 6.0f});
  service_->DrainCommandQueueForTests();
  EXPECT_TRUE(latch_->running());
  EXPECT_EQ(LiveTranslation(), first);
  EXPECT_TRUE(service_->mask_creation_commands_pending());
  EXPECT_EQ(PendingAppendCount(), 1U);
  const auto coalesced = LastPendingAppend();
  ASSERT_TRUE(coalesced.has_value());
  EXPECT_EQ(coalesced->sample.reference_pixels.x, 20.0f);
  EXPECT_EQ(coalesced->sample.reference_pixels.y, 6.0f);
  EXPECT_FALSE(coalesced->ordered_append);

  latch_->Complete(true);
  service_->DrainCommandQueueForTests();
  EXPECT_EQ(LiveTranslation(), first);
  clock_->clock.set_ns(16'000'000);
  ConsumeQueued();
  ASSERT_TRUE(latch_->running());
  const auto latest = BrushPlacementForReferenceDrag({0.0f, 0.0f}, {8.0f, 6.0f}, {20.0f, 6.0f});
  EXPECT_EQ(LiveTranslation(), latest);
  EXPECT_FALSE(service_->mask_creation_commands_pending());
}

TEST_F(SerialMaskInteractiveTest, LatestMoveValueSurvivesRelease) {
  OpenInteractive();
  EnqueueSelectAndArmMove();
  EnqueueAppend({10.0f, 6.0f});
  EnqueueAppend({14.0f, 7.0f});
  EnqueueAppend({18.0f, 8.0f});
  EditorMaskCreationCommand finish;
  finish.kind    = EditorMaskCreationCommandKind::Finish;
  finish.node_id = GradeId();
  finish.mask_id = kMask;
  Enqueue(finish);

  const auto pending = service_->PeekPendingMaskCommands();
  ASSERT_EQ(PendingAppendCount(), 1U);
  const auto coalesced = LastPendingAppend();
  ASSERT_TRUE(coalesced.has_value());
  EXPECT_EQ(coalesced->sample.reference_pixels.x, 18.0f);
  EXPECT_EQ(coalesced->sample.reference_pixels.y, 8.0f);
  EXPECT_FALSE(coalesced->ordered_append);
  EXPECT_EQ(pending.back().kind, EditorMaskCreationCommandKind::Finish);

  ConsumeQueued();
  ASSERT_TRUE(latch_->running());
  ASSERT_FALSE(latch_->scheduled().empty());
  EXPECT_EQ(latch_->scheduled().back().intent.quality, EditorRenderQuality::Quality);
  const auto latest = BrushPlacementForReferenceDrag({0.0f, 0.0f}, {8.0f, 6.0f}, {18.0f, 8.0f});
  EXPECT_EQ(LiveTranslation(), latest);
  EXPECT_FALSE(service_->mask_creation_commands_pending());
}

}  // namespace
}  // namespace alcedo
