//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file editor_history_move_during_input_test.cpp
/// @brief Undo, Redo, history moves, and Paste are refused while a slider input sequence or a
///        Mask edit is open, through the production session facade over the real history port.
///        An uncommitted value belongs to the editor session only; a history move on such a
///        document would publish it as committed.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "app/editor_action_policy.hpp"
#include "app/editor_mask_creation_controller.hpp"
#include "app/editor_session_bootstrap.hpp"
#include "app/editor_session_service.hpp"
#include "app/pipeline_root_state.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_graph.hpp"
#include "json.hpp"
#include "support/editor_history_port_test_reads.hpp"
#include "support/editor_lease_test_support.hpp"
#include "support/editor_parameter_target_test.hpp"
#include "support/editor_parameter_write_test.hpp"
#include "support/editor_session_test_ports.hpp"
#include "ui/alcedo_main/album_backend/editor_session_history_port.hpp"
#include "ui/alcedo_main/album_backend/editor_session_pipeline_port.hpp"

namespace alcedo::ui {
namespace {

constexpr sl_element_id_t kElementId = 52;
constexpr image_id_t      kImageId   = 520;

auto                      HistoryMoveTestPath(std::string_view name) -> std::filesystem::path {
  const auto stamp =
      std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
  auto dir = std::filesystem::path{"build/tmp/editor_history_move_during_input"};
  std::filesystem::create_directories(dir);
  return dir / (std::string{name} + "_" + stamp + ".wal");
}

/// Records editor frame requests; frames complete only when the test reports them.
class RecordingScheduler final : public IEditorPipelineSchedulerPort {
 public:
  auto Schedule(const EditorRenderRequest& request,
                EditorPipelineScheduleCompletion /*on_complete*/) -> std::uint64_t override {
    scheduled.push_back(request);
    return ++next_job;
  }
  void                             Cancel(std::uint64_t /*job_id*/) override {}

  std::vector<EditorRenderRequest> scheduled;
  std::uint64_t                    next_job = 0;
};

auto MaskSample(float x, float y) -> MaskCreationSample {
  MaskCreationSample sample;
  sample.normalized        = {x, y};
  sample.reference_pixels  = {x * 640.0f, y * 480.0f};
  sample.inside_photograph = true;
  return sample;
}

class EditorHistoryMoveDuringInputTest : public ::testing::Test {
 protected:
  void SetUp() override {
    journal_path_ = HistoryMoveTestPath("history_move_during_input");
    lease_    = test::MakeInMemoryEditorLease(kElementId, test::WorkingSpaceBoundDefaultDocument());
    pipeline_ = std::make_shared<EditorSessionPipelinePort>();
    pipeline_->SetServices(EditorSessionPipelineMappers{
        {}, [this](sl_element_id_t) { return test::CopyEditorLease(lease_); }});
    history_ = std::make_shared<EditorSessionHistoryPort>();
    history_->SetServices(
        EditorSessionHistoryPort::Services{[this](sl_element_id_t) { return journal_path_; }});
    history_->SetPipelinePort(pipeline_);
    scheduler_ = std::make_shared<RecordingScheduler>();
    runtime_   = EditorSessionRuntime::CreateWithPorts(
        pipeline_, history_, std::make_shared<test::FakeEditorTaskPort>(), scheduler_,
        std::make_shared<test::FakeEditorCheckpointStore>(),
        std::make_shared<test::FakeEditorThumbnailPort>());
    service_ = runtime_->service.get();
    service_->SetPresentationSinkId(1);
    service_->SetPresentationSize(640, 480);
    Drain();
  }

  void TearDown() override {
    runtime_.reset();
    std::error_code ec;
    std::filesystem::remove(journal_path_, ec);
  }

  void Drain() { service_->DrainCommandQueueForTests(); }

  /// Completes every frame the coordinator schedules until no new frame follows.
  void CompleteFrames() {
    Drain();
    std::size_t completed = 0;
    while (completed < scheduler_->scheduled.size()) {
      const auto request_id = scheduler_->scheduled[completed].request_id;
      ++completed;
      runtime_->coordinator->NotifySchedulerCompleted(request_id, true);
      Drain();
    }
  }

  void OpenInteractive() {
    (void)service_->Open(kElementId, kImageId);
    CompleteFrames();
    ASSERT_EQ(service_->state(), EditorSessionState::Interactive);
  }

  /// Two settled exposure commits through the session, then one Undo, so Undo, Redo, and a
  /// head move are all available before the input sequence opens.
  void CommitTwoExposureEditsAndUndoOne() {
    for (const auto* json : {R"({"exposure":0.5})", R"({"exposure":1.0})"}) {
      ASSERT_EQ(service_
                    ->CommitAdjustment(
                        test::WithColorGradeTarget(test::PatchFromJson("exposure", json, true)))
                    .kind,
                EditorSessionResultKind::RenderRouted);
      CompleteFrames();
    }
    redo_target_ = Head();
    ASSERT_TRUE(redo_target_.has_value());
    ASSERT_EQ(service_->Undo().kind, EditorSessionResultKind::Accepted) << LastMessage();
    CompleteFrames();
    // Paste is denied by default only because no package was copied; make it available so its
    // decision shows the input-sequence rule.
    service_->SetCopiedPackageAvailable(true);
    Drain();
    for (const auto action : {EditorAction::Undo, EditorAction::Redo, EditorAction::MoveHead,
                              EditorAction::ApplyPaste}) {
      ASSERT_TRUE(Decision(action).allowed) << EditorActionName(action);
    }
  }

  /// Queue one contrast write as the slider sends it and let the session apply it.
  void EnqueueContrast(const char* json, bool settled) {
    ASSERT_EQ(service_
                  ->EnqueueAdjustmentInput(
                      test::WithColorGradeTarget(test::PatchFromJson("contrast", json, settled)))
                  .kind,
              EditorSessionResultKind::Accepted);
    CompleteFrames();
  }

  /// Hold a Radial Mask creation drag open, as a held pointer does.
  void OpenRadialMaskDrag() {
    const MaskPointerIdentity pointer{3, 1, 9};
    EditorMaskCreationCommand begin_mode;
    begin_mode.kind        = EditorMaskCreationCommandKind::BeginCreation;
    begin_mode.source_kind = MaskSourceKind::Radial;
    begin_mode.node_id     = Working()->Document().DefaultGradeId();
    EditorMaskCreationCommand press;
    press.kind        = EditorMaskCreationCommandKind::BeginInput;
    press.source_kind = MaskSourceKind::Radial;
    press.sample      = MaskSample(0.40f, 0.40f);
    press.identity    = pointer;
    EditorMaskCreationCommand drag;
    drag.kind     = EditorMaskCreationCommandKind::Append;
    drag.sample   = MaskSample(0.60f, 0.55f);
    drag.identity = pointer;
    ASSERT_EQ(service_->EnqueueMaskCreation(begin_mode).kind, EditorSessionResultKind::Accepted);
    ASSERT_EQ(service_->EnqueueMaskCreation(press).kind, EditorSessionResultKind::Accepted);
    ASSERT_EQ(service_->EnqueueMaskCreation(drag).kind, EditorSessionResultKind::Accepted);
    CompleteFrames();
  }

  auto Graph() -> std::shared_ptr<const CommitGraph> {
    return test::EditorHistoryGraph(*history_, kElementId);
  }
  auto Head() -> head_commit_hash_t { return Graph()->GetActiveVersionRef().head_commit_hash; }
  auto Working() -> std::shared_ptr<const PipelineGraphSnapshot> {
    return test::EditorWorkingPreview(*pipeline_, kElementId);
  }
  /// Document of the active head, replayed from the immutable root.
  auto HeadReplayJson() -> nlohmann::json {
    std::string error;
    const auto  graph  = Graph();
    const auto  replay = BuildDocumentFromRoot(
        *graph, *lease_.root_, graph->GetActiveVersionRef().head_commit_hash, &error);
    EXPECT_NE(replay, nullptr) << error;
    return replay ? replay->ToJson() : nlohmann::json{};
  }
  auto Decision(EditorAction action) const -> EditorActionDecision {
    return service_->action_availability().For(action);
  }
  auto LastMessage() const -> std::string {
    const auto results = service_->results();
    return results.empty() ? std::string{} : results.back().message;
  }

  /// Every history move and Paste is refused, by the projected decision and at admission, and
  /// neither the head nor the working document changes.
  void ExpectHistoryMovesRefused(const char* input) {
    for (const auto action : {EditorAction::Undo, EditorAction::Redo, EditorAction::MoveHead,
                              EditorAction::ApplyPaste}) {
      EXPECT_FALSE(Decision(action).allowed) << input << ": " << EditorActionName(action);
    }
    const auto head_before    = Head();
    const auto working_before = Working()->Document().ToJson();

    EXPECT_EQ(service_->Undo().kind, EditorSessionResultKind::Rejected) << input;
    EXPECT_EQ(service_->Redo().kind, EditorSessionResultKind::Rejected) << input;
    EXPECT_EQ(service_->MoveHeadToCommit(*redo_target_).kind, EditorSessionResultKind::Rejected)
        << input;
    CompleteFrames();
    EXPECT_EQ(Head(), head_before) << input;
    EXPECT_EQ(Working()->Document().ToJson(), working_before) << input;
  }

  std::filesystem::path                      journal_path_;
  EditorHistoryLease                         lease_;
  std::shared_ptr<EditorSessionPipelinePort> pipeline_;
  std::shared_ptr<EditorSessionHistoryPort>  history_;
  std::shared_ptr<RecordingScheduler>        scheduler_;
  std::unique_ptr<EditorSessionRuntime>      runtime_;
  EditorSessionService*                      service_ = nullptr;
  /// Commit that Undo left in the redo suffix: the target of the refused head move.
  head_commit_hash_t                         redo_target_;
};

TEST_F(EditorHistoryMoveDuringInputTest, UndoIsDeniedWhileASliderInputSequenceIsOpen) {
  OpenInteractive();
  CommitTwoExposureEditsAndUndoOne();

  // A held slider drag: the preview value is on the working document, the sequence is open.
  EnqueueContrast(R"({"contrast":40.0})", false);
  ASSERT_FALSE(service_->PeekPendingInput().sequences.empty());
  ExpectHistoryMovesRefused("open slider input");

  // The release commits the sequence; history moves are available again. The new commit ends the
  // redo suffix, so only Redo stays off.
  EnqueueContrast(R"({"contrast":40.0})", true);
  EXPECT_TRUE(service_->PeekPendingInput().sequences.empty());
  for (const auto action : {EditorAction::Undo, EditorAction::MoveHead, EditorAction::ApplyPaste}) {
    EXPECT_TRUE(Decision(action).allowed) << EditorActionName(action);
  }
  EXPECT_EQ(Decision(EditorAction::Redo).reason, "Nothing to redo");
}

TEST_F(EditorHistoryMoveDuringInputTest, UndoIsDeniedWhileAMaskEditIsOpen) {
  OpenInteractive();
  CommitTwoExposureEditsAndUndoOne();

  OpenRadialMaskDrag();
  ASSERT_NE(Working()->Document().PrimaryGrade()->MaskCount(), 0u)
      << "the provisional Mask is on the working document before release";
  ExpectHistoryMovesRefused("open Mask edit");
}

TEST_F(EditorHistoryMoveDuringInputTest,
       ArrowKeyThenUndoWithinDebounceKeepsDocumentEqualToHeadReplay) {
  OpenInteractive();
  CommitTwoExposureEditsAndUndoOne();
  const auto head_before = Head();

  // The arrow key sends a preview write; Ctrl+Z arrives before the 180 ms debounce sends the
  // settled write of the same value.
  EnqueueContrast(R"({"contrast":40.0})", false);
  EXPECT_EQ(service_->Undo().kind, EditorSessionResultKind::Rejected);
  CompleteFrames();
  EnqueueContrast(R"({"contrast":40.0})", true);

  ASSERT_NE(Head(), head_before) << "the settled write records its own commit";
  EXPECT_EQ(Graph()->FirstParentChain(Head()).size(),
            Graph()->FirstParentChain(head_before).size() + 1);
  EXPECT_EQ(Working()->Document().ToJson(), HeadReplayJson())
      << "the working document holds a value that no commit records";

  // Undo of that commit restores exactly the state before the arrow key.
  ASSERT_EQ(service_->Undo().kind, EditorSessionResultKind::Accepted) << LastMessage();
  CompleteFrames();
  EXPECT_EQ(Head(), head_before);
  EXPECT_EQ(Working()->Document().ToJson(), HeadReplayJson());
}

}  // namespace
}  // namespace alcedo::ui
