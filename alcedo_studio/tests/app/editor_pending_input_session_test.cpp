//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "app/editor_pending_input.hpp"
#include "app/editor_render_coordinator.hpp"
#include "app/editor_session_bootstrap.hpp"
#include "app/editor_session_service.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "edit/history/pipeline_edit_batch.hpp"
#include "support/editor_parameter_write_test.hpp"
#include "support/editor_session_command_queue_test_support.hpp"

namespace alcedo {
namespace {

class RecordingScheduler final : public IEditorPipelineSchedulerPort {
 public:
  auto Schedule(const EditorRenderRequest&, EditorPipelineScheduleCompletion = {})
      -> std::uint64_t override {
    return ++next_job_;
  }
  void Cancel(std::uint64_t) override {}

 private:
  std::uint64_t next_job_ = 0;
};

class EditorPendingInputSessionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    history_          = std::make_shared<test::FakeEditorHistoryPort>();
    pipeline_         = std::make_shared<test::FakeEditorPipelinePort>();
    tasks_            = std::make_shared<test::FakeEditorTaskPort>();
    scheduler_        = std::make_shared<RecordingScheduler>();
    checkpoint_store_ = std::make_shared<test::FakeEditorCheckpointStore>();
    runtime_          = EditorSessionRuntime::CreateWithPorts(
        pipeline_, history_, tasks_, scheduler_, checkpoint_store_);
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

TEST_F(EditorPendingInputSessionTest, EmptyNodeSwitchDoesNotBumpHistoryRevision) {
  OpenInteractive();
  const auto revision = service_->history_revision();
  const int  captures = history_->capture_count;
  const int  commits  = history_->commit_count;
  ASSERT_EQ(service_->EnqueuePendingInputBoundary(EditorPendingInputBoundaryKind::NodeSwitch).kind,
            EditorSessionResultKind::Accepted);
  service_->DrainCommandQueueForTests();
  EXPECT_EQ(service_->history_revision(), revision);
  EXPECT_EQ(history_->capture_count, captures);
  EXPECT_EQ(history_->commit_count, commits);
  EXPECT_TRUE(service_->PeekPendingInput().sequences.empty());
}

TEST_F(EditorPendingInputSessionTest, SelectedNodeProjectionDoesNotCaptureOrCommit) {
  OpenInteractive();
  const auto revision = service_->history_revision();
  const int  captures = history_->capture_count;
  const int  commits  = history_->commit_count;
  const auto result   = service_->SetAdjustmentProjectionNode(NodeId{"grade.b"});
  service_->DrainCommandQueueForTests();
  EXPECT_EQ(result.kind, EditorSessionResultKind::Accepted);
  EXPECT_EQ(history_->set_panel_projection_node_count, 1);
  EXPECT_EQ(history_->last_panel_projection_node, NodeId{"grade.b"});
  EXPECT_EQ(history_->last_panel_projection_generation,
            service_->active_image_load_request().value);
  EXPECT_EQ(service_->history_revision(), revision);
  EXPECT_EQ(history_->capture_count, captures);
  EXPECT_EQ(history_->commit_count, commits);
}

TEST_F(EditorPendingInputSessionTest, SelectedNodeProjectionDoesNotWaitForInflightFrame) {
  (void)service_->Open(10, 20);
  service_->DrainCommandQueueForTests();
  const auto first_rid = service_->first_frame_request_id();
  ASSERT_NE(first_rid, 0u);
  runtime_->coordinator->NotifySchedulerCompleted(first_rid, true);
  service_->DrainCommandQueueForTests();
  ASSERT_EQ(service_->state(), EditorSessionState::Interactive);
  ASSERT_TRUE(runtime_->coordinator->has_inflight());
  ASSERT_NE(runtime_->coordinator->last_scheduled_request_id(), first_rid);

  const auto result = service_->SetAdjustmentProjectionNode(NodeId{"grade.b"});
  service_->DrainCommandQueueForTests();
  EXPECT_EQ(result.kind, EditorSessionResultKind::Accepted);
  EXPECT_EQ(history_->set_panel_projection_node_count, 1);
  EXPECT_EQ(history_->last_panel_projection_node, NodeId{"grade.b"});
  EXPECT_EQ(history_->last_panel_projection_generation,
            service_->active_image_load_request().value);
  EXPECT_TRUE(runtime_->coordinator->has_inflight());
}

/// Records how many edits were committed when the save seal captured history.
class SealOrderHistoryPort final : public test::FakeEditorHistoryPort {
 public:
  int  commits_at_capture = -1;

  auto CaptureSaveCheckpoint(const EditorHistoryGuardHandle& guard, std::string* error)
      -> std::shared_ptr<const EditorMiniGitSaveCapture> override {
    commits_at_capture = commit_count;
    return FakeEditorHistoryPort::CaptureSaveCheckpoint(guard, error);
  }
};

class EditorPendingInputSealTest : public EditorPendingInputSessionTest {
 protected:
  void SetUp() override {
    EditorPendingInputSessionTest::SetUp();
    ordered_history_ = std::make_shared<SealOrderHistoryPort>();
    history_         = ordered_history_;
    runtime_ = EditorSessionRuntime::CreateWithPorts(pipeline_, history_, tasks_, scheduler_,
                                                     checkpoint_store_);
    service_ = runtime_->service.get();
    service_->SetPresentationSinkId(1);
    service_->SetPresentationSize(640, 480);
  }

  /// Leaves a settled crop edit queued behind an in-flight preview frame, the
  /// state in which pacing defers its consume.
  void QueueSettledEditBehindInflightFrame() {
    OpenInteractive();
    ASSERT_EQ(service_->EnqueueAdjustmentInput(test::ScalarPatch("exposure", 0.25f, false)).kind,
              EditorSessionResultKind::Accepted);
    service_->DrainCommandQueueForTests();
    ASSERT_TRUE(runtime_->coordinator->has_inflight());
    EditorAdjustmentPatch settled = test::ScalarPatch("exposure", 0.40f, true);
    ASSERT_EQ(service_->EnqueueAdjustmentInput(settled).kind, EditorSessionResultKind::Accepted);
    service_->DrainCommandQueueForTests();
    ASSERT_EQ(history_->commit_count, 0);
    ASSERT_FALSE(service_->PeekPendingInput().sequences.empty());
  }

  std::shared_ptr<SealOrderHistoryPort> ordered_history_;
};

TEST_F(EditorPendingInputSealTest, SwitchCommitsQueuedEditBeforeTheSaveSeal) {
  QueueSettledEditBehindInflightFrame();

  (void)service_->Switch(11, 21);
  service_->DrainCommandQueueForTests();

  // The deferred edit became this image's history before the capture, and
  // nothing of it is left to be consumed against the next image.
  EXPECT_EQ(history_->commit_count, 1);
  EXPECT_EQ(ordered_history_->commits_at_capture, 1);
  ASSERT_TRUE(history_->last_committed_patch.write.has_value());
  EXPECT_TRUE(service_->PeekPendingInput().sequences.empty());
}

TEST_F(EditorPendingInputSealTest, SwitchIsRefusedWithTheRealErrorWhenTheQueuedEditCannotCommit) {
  QueueSettledEditBehindInflightFrame();
  history_->fail_commit = true;

  (void)service_->Switch(11, 21);
  service_->DrainCommandQueueForTests();

  // Fail closed: no capture of a partial state, the image stays open, and the
  // retained failure names the cause instead of a generic save error.
  EXPECT_EQ(history_->checkpoint_capture_count, 0);
  EXPECT_EQ(service_->identity().element_id, static_cast<sl_element_id_t>(10));
  EXPECT_NE(service_->last_error().find("mini-Git journal append failed"), std::string::npos)
      << service_->last_error();
}

/// History port with a real live document for locked-document (Mask) input. Records each settle
/// and the input-sequence state the session reports, and what had settled when the save seal
/// captured history.
class MaskDocumentHistoryPort final : public test::FakeEditorHistoryPort {
 public:
  PipelineDocument             document = CreateDefaultPipelineDocument();
  std::shared_ptr<CommitGraph> graph = std::make_shared<CommitGraph>(CommitGraph::CreateEmpty(10));
  std::shared_ptr<MiniGitJournal>        journal = std::make_shared<MiniGitJournal>();
  MiniGitWorkingHistory                  history{graph, journal};
  bool                                   input_open = false;
  std::vector<PipelineEditOperationKind> settled;
  int                                    settled_at_capture    = -1;
  bool                                   input_open_at_capture = false;

  auto WithWorkingDocument(const EditorHistoryGuardHandle&, const MaskDocumentOp& op,
                           std::string* error) -> bool override {
    MaskSettle settle = [this](const PipelineEditBatch& batch, std::string*) {
      settled.push_back(batch.operation_kind);
      return true;
    };
    return op(document, history, settle, &input_open, error);
  }

  auto CaptureSaveCheckpoint(const EditorHistoryGuardHandle& guard, std::string* error)
      -> std::shared_ptr<const EditorMiniGitSaveCapture> override {
    settled_at_capture    = static_cast<int>(settled.size());
    input_open_at_capture = input_open;
    return FakeEditorHistoryPort::CaptureSaveCheckpoint(guard, error);
  }
};

class EditorMaskInputSealTest : public EditorPendingInputSessionTest {
 protected:
  void SetUp() override {
    EditorPendingInputSessionTest::SetUp();
    mask_history_ = std::make_shared<MaskDocumentHistoryPort>();
    history_      = mask_history_;
    runtime_      = EditorSessionRuntime::CreateWithPorts(pipeline_, history_, tasks_, scheduler_,
                                                          checkpoint_store_);
    service_      = runtime_->service.get();
    service_->SetPresentationSinkId(1);
    service_->SetPresentationSize(640, 480);
  }

  static auto Sample(float x, float y) -> MaskCreationSample {
    MaskCreationSample sample;
    sample.normalized        = {x, y};
    sample.reference_pixels  = {x * 640.0f, y * 480.0f};
    sample.inside_photograph = true;
    return sample;
  }

  /// Start a Radial creation drag and leave it open, as a held pointer does.
  void OpenRadialCreationDrag() {
    OpenInteractive();
    const MaskPointerIdentity pointer{3, 1, 9};
    EditorMaskCreationCommand begin_mode;
    begin_mode.kind        = EditorMaskCreationCommandKind::BeginCreation;
    begin_mode.source_kind = MaskSourceKind::Radial;
    begin_mode.node_id     = mask_history_->document.DefaultGradeId();
    EditorMaskCreationCommand press;
    press.kind        = EditorMaskCreationCommandKind::BeginInput;
    press.source_kind = MaskSourceKind::Radial;
    press.sample      = Sample(0.40f, 0.40f);
    press.identity    = pointer;
    EditorMaskCreationCommand drag;
    drag.kind     = EditorMaskCreationCommandKind::Append;
    drag.sample   = Sample(0.60f, 0.55f);
    drag.identity = pointer;
    ASSERT_EQ(service_->EnqueueMaskCreation(begin_mode).kind, EditorSessionResultKind::Accepted);
    ASSERT_EQ(service_->EnqueueMaskCreation(press).kind, EditorSessionResultKind::Accepted);
    ASSERT_EQ(service_->EnqueueMaskCreation(drag).kind, EditorSessionResultKind::Accepted);
    service_->DrainCommandQueueForTests();
  }

  std::shared_ptr<MaskDocumentHistoryPort> mask_history_;
};

TEST_F(EditorMaskInputSealTest, OpenMaskDragIsReportedAsUncommittedInput) {
  OpenRadialCreationDrag();
  EXPECT_TRUE(mask_history_->input_open);
  EXPECT_TRUE(mask_history_->settled.empty());
  EXPECT_EQ(mask_history_->document.PrimaryGrade()->MaskCount(), 1u)
      << "the provisional Mask is on the live document before release";
}

TEST_F(EditorMaskInputSealTest, PersistCommitsAnOpenMaskDragBeforeTheSaveSealLikeASliderDrag) {
  OpenRadialCreationDrag();
  ASSERT_TRUE(mask_history_->input_open);

  (void)service_->PersistCurrentImage();
  service_->DrainCommandQueueForTests();

  // The boundary finished the drag as a pointer release would: one AddMask commit, recorded
  // before the capture, and no input left open on the live document.
  ASSERT_EQ(mask_history_->settled.size(), 1u);
  EXPECT_EQ(mask_history_->settled.front(), PipelineEditOperationKind::AddMask);
  EXPECT_EQ(mask_history_->settled_at_capture, 1);
  EXPECT_FALSE(mask_history_->input_open_at_capture);
  EXPECT_FALSE(mask_history_->input_open);
}

}  // namespace
}  // namespace alcedo
