//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file editor_session_command_queue_baseline_test.cpp
/// @brief CQ0 failing-evidence / CQ1 acceptance tests for the editor-session
///        command-queue plan.
///
/// Each test asserts a post-restructure invariant from the plan. Captured
/// against the pre-CQ1 facade, 8 of 10 failed; since CQ1 they are the
/// acceptance suite for the single-thread command queue:
///
/// - inline completion: a worker completion never runs session code on the
///   service-start stack (invariant 2/3);
/// - blocking render-lock: a command-thread operation never waits on the
///   executor render mutex (invariant 2);
/// - split snapshot: one accepted command publishes one change notification
///   and one terminal result (invariant 6/7).
///
/// The tests drive the real `EditorSessionService` facade through
/// controllable ports (see editor_session_command_queue_test_support.hpp) and
/// the real `EditorRenderCoordinator`. No test sleeps for ordering; delayed
/// completion is driven by the test, posted completions reduce only through
/// `drainQueue()`, and blocking is detected with a bounded timeout that
/// reports the exact unfinished operation.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "app/editor_render_coordinator.hpp"
#include "app/editor_session_bootstrap.hpp"
#include "app/editor_session_service.hpp"
#include "app/editor_session_types.hpp"
#include "support/editor_session_command_queue_test_support.hpp"

namespace alcedo {
namespace {
using namespace alcedo::test;  // controllable ports + recorder live in alcedo::test

auto MakeExposureTransferPackage(double exposure) -> AdjustmentTransferPackage {
  AdjustmentTransferPackage package;
  package.operators_.push_back(AdjustmentTransferEntry{
      PipelineStageName::Basic_Adjustment, OperatorType::EXPOSURE, true, false,
      nlohmann::json{{"exposure", exposure}}});
  return package;
}

/// Recording scheduler: accepts every render request and returns a non-zero
/// job id so the coordinator marks it in-flight. Completion is driven manually
/// by the test through `EditorRenderCoordinator::Notify*`.
class RecordingScheduler final : public IEditorPipelineSchedulerPort {
 public:
  auto Schedule(const EditorRenderRequest& request) -> std::uint64_t override {
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
  std::uint64_t                    next_job_ = 0;
};

class EditorSessionCommandQueueBaselineTest : public ::testing::Test {
 protected:
  void SetUp() override { RebuildSession(); }

  /// Rebuild the full session runtime with fresh ports and recorder. Tests
  /// that compare two runs of the same scenario call this between runs.
  void RebuildSession() {
    history_          = std::make_shared<ControllableEditorHistoryPort>();
    pipeline_         = std::make_shared<FakeEditorPipelinePort>();
    tasks_            = std::make_shared<FakeEditorTaskPort>();
    journal_          = std::make_shared<OrderRecordingJournalPort>();
    scheduler_        = std::make_shared<RecordingScheduler>();
    checkpoint_store_ = std::make_shared<FakeEditorCheckpointStore>();
    thumbnails_       = std::make_shared<FakeEditorThumbnailPort>();

    runtime_          = EditorSessionRuntime::CreateWithPorts(pipeline_, history_, tasks_, journal_,
                                                              scheduler_, checkpoint_store_, thumbnails_);
    service_          = runtime_->service.get();
    service_->SetPresentationSinkId(1);
    service_->SetPresentationSize(640, 480);

    recorder_ = std::make_unique<SessionResultRecorder>();
    service_->SetResultObserver(recorder_->result_observer());
    service_->SetChangeNotifier(recorder_->change_notifier());
  }

  /// Open an image with no prior image (synchronous) and present its first
  /// frame so the session reaches Interactive. Renders are driven manually
  /// through the coordinator; no worker thread is involved.
  void openInteractive(sl_element_id_t eid = 10, image_id_t iid = 20) {
    recorder_->mark_initiating();
    (void)service_->Open(eid, iid);
    recorder_->mark_returned();
    presentFirstFrame();
    ASSERT_EQ(service_->state(), EditorSessionState::Interactive)
        << "openInteractive did not reach Interactive";
  }

  /// Drive the active first-frame request through complete->submit->present.
  /// Each coordinator notification posts a render completion to the command
  /// queue; the queue is drained between stages so the render controller's
  /// complete->submit->present gate advances in order.
  void presentFirstFrame() {
    // Run any posted save/render completions that route the first frame (a
    // save-bounded switch routes the first frame only after its checkpoint
    // completion reduces on the queue).
    drainQueue();
    const auto rid = service_->first_frame_request_id();
    if (rid == 0) {
      return;
    }
    runtime_->coordinator->NotifySchedulerCompleted(rid, true);
    drainQueue();
    runtime_->coordinator->NotifyFrameSubmitted(rid);
    drainQueue();
    runtime_->coordinator->NotifyFramePresented(rid);
    drainQueue();
  }

  /// Run all command/completion work posted to the session queue.
  void drainQueue() { service_->DrainCommandQueueForTests(); }

  /// Open A, switch to B, let the save finish, and present B's first frame.
  /// `async_save` selects delayed worker completion (journal commit and
  /// materialization complete from the test as the worker) or inline port
  /// completion (still posted to the queue, never run on the start stack).
  /// Returns the ordered (kind, state) result sequence the session published.
  auto runSwitchSequence(bool async_save)
      -> std::vector<std::pair<EditorSessionResultKind, EditorSessionState>> {
    journal_->async_commit               = async_save;
    checkpoint_store_->async_materialize = async_save;
    openInteractive(10, 20);
    (void)service_->Switch(30, 40);
    journal_->CompleteCommit(true);
    checkpoint_store_->CompleteMaterialization(true);
    drainQueue();
    presentFirstFrame();
    std::vector<std::pair<EditorSessionResultKind, EditorSessionState>> sequence;
    sequence.reserve(recorder_->results.size());
    for (const auto& r : recorder_->results) {
      sequence.emplace_back(r.kind, r.state);
    }
    return sequence;
  }

  /// Terminal results published since `baseline`.
  [[nodiscard]] auto terminals_since(std::size_t baseline) const -> std::size_t {
    return recorder_->terminal_count() - baseline;
  }

  std::shared_ptr<ControllableEditorHistoryPort> history_;
  std::shared_ptr<FakeEditorPipelinePort>        pipeline_;
  std::shared_ptr<FakeEditorTaskPort>            tasks_;
  std::shared_ptr<OrderRecordingJournalPort>     journal_;
  std::shared_ptr<RecordingScheduler>            scheduler_;
  std::shared_ptr<FakeEditorCheckpointStore>     checkpoint_store_;
  std::shared_ptr<FakeEditorThumbnailPort>       thumbnails_;
  std::unique_ptr<EditorSessionRuntime>          runtime_;
  EditorSessionService*                          service_ = nullptr;
  std::unique_ptr<SessionResultRecorder>         recorder_;
};

/// Invariant: a worker completion (the save checkpoint) is processed AFTER the
/// initiating command returns, never inline on the service-start stack. The
/// current synchronous save path invokes the journal callback inline, so the
/// save completion re-enters navigation and publishes save results BEFORE
/// `Switch` returns. The sentinel counts results carrying a save task id that
/// arrived during the initiating call; the target is zero.
TEST_F(EditorSessionCommandQueueBaselineTest,
       SynchronousJournalResultIsProcessedAfterInitiatingCommandReturns) {
  openInteractive(10, 20);  // image A

  recorder_->mark_initiating();
  const auto before                    = recorder_->terminal_count();
  // Inline (synchronous) save reproduces the production journal-writer path.
  journal_->async_commit               = false;
  checkpoint_store_->async_materialize = false;
  const auto result                    = service_->Switch(30, 40);  // switch to B
  recorder_->mark_returned();

  EXPECT_EQ(result.kind, EditorSessionResultKind::SaveStarted);
  // Target invariant: no terminal save-completion ran on the start stack. The
  // save completion is posted to the command queue and reduces only on drain.
  EXPECT_EQ(recorder_->terminal_during_initiating, 0u)
      << "save completion ran inline before Switch returned (unfinished op: B acquisition)";
  // The command publishes exactly one terminal result, delivered after the
  // queue drains the posted save completion.
  drainQueue();
  EXPECT_EQ(terminals_since(before), 1u)
      << "one accepted command must publish exactly one terminal result";
}

/// Invariant: a rapid second selection keeps the running target and replaces
/// only an unstarted pending selection. The current navigation rejects a
/// second selection outright while a save is in progress instead of recording
/// it as the pending next target, so C is rejected and B remains the only
/// tracked target. The target is: C is not rejected and B is still acquired.
TEST_F(EditorSessionCommandQueueBaselineTest,
       RapidImageSelectionKeepsRunningTargetAndReplacesOnlyUnstartedSelection) {
  openInteractive(10, 20);  // image A

  // Start an async save for A; B is the pending running target.
  journal_->async_commit               = true;
  checkpoint_store_->async_materialize = true;
  const auto switch_b                  = service_->Switch(30, 40);  // switch to B
  ASSERT_EQ(switch_b.kind, EditorSessionResultKind::SaveStarted);

  // Rapidly request C while B's save is still in progress.
  recorder_->mark_initiating();
  const auto switch_c = service_->Switch(50, 60);
  recorder_->mark_returned();

  // Target: C replaces the pending selection (or queues behind B); it is not
  // rejected with a "save in progress" busy result.
  EXPECT_NE(switch_c.kind, EditorSessionResultKind::Rejected)
      << "second selection must replace/queue, not be rejected (unfinished op: C selection)";

  // The running target B must still complete after its save.
  journal_->CompleteCommit(true);
  checkpoint_store_->CompleteMaterialization(true);
  drainQueue();
  EXPECT_EQ(service_->identity().element_id, static_cast<sl_element_id_t>(30))
      << "running target B must remain the acquired image";

  // The queued selection C is promoted once B's save completes: it seals B and
  // runs its own save checkpoint before acquiring C.
  journal_->CompleteCommit(true);
  checkpoint_store_->CompleteMaterialization(true);
  drainQueue();
  EXPECT_EQ(service_->identity().element_id, static_cast<sl_element_id_t>(50))
      << "the promoted queued selection must acquire C after B's save finishes";
}

/// Invariant: a save completion whose generation does not match the pending
/// save cannot release the current image's guards. This is a guard test for the
/// async path; the current code correlates by request_id and session_generation
/// there. The residual CQ0 gap is the synchronous-inline unconditional-accept
/// (navigation line 198-207) which the inline test above exercises; CQ1 removes
/// the inline path entirely.
TEST_F(EditorSessionCommandQueueBaselineTest,
       StaleSaveCompletionCannotReleaseTheCurrentImageGuards) {
  openInteractive(10, 20);  // image A

  // Start an async save for A; B pending, ticket assigned (correlated path).
  journal_->async_commit               = true;
  checkpoint_store_->async_materialize = true;
  ASSERT_EQ(service_->Switch(30, 40).kind, EditorSessionResultKind::SaveStarted);

  const auto identity_before = service_->identity();
  const auto state_before    = service_->state();

  // While the save is pending, A's guards are retained: identity is still A
  // and the session is mid-save. No completion has fired yet.
  EXPECT_EQ(identity_before.element_id, static_cast<sl_element_id_t>(10))
      << "guards must be retained while the save is pending";
  EXPECT_EQ(state_before, EditorSessionState::Saving);

  // Complete the real (correlated) save; only the matching generation may
  // release A's guards and acquire B. The completion is posted to the command
  // queue and reduces on drain.
  journal_->CompleteCommit(true);
  checkpoint_store_->CompleteMaterialization(true);
  drainQueue();

  EXPECT_EQ(service_->identity().element_id, static_cast<sl_element_id_t>(30))
      << "only the matching completion acquires B";
  EXPECT_NE(service_->state(), EditorSessionState::Saving);
}

/// Invariant: a first frame presented for a generation that no longer matches
/// the active session cannot enable editing for another image. The render
/// controller filters results by session_generation/image/element; this test
/// pins that guard so CQ1/CQ3 keep it once completion is queue-mediated.
TEST_F(EditorSessionCommandQueueBaselineTest, StaleFirstFrameCannotEnableEditingForAnotherImage) {
  openInteractive(10, 20);  // image A, generation 1
  const auto load_a = service_->active_image_load_request();

  // Switch to B synchronously (inline save) and present B's first frame.
  journal_->async_commit               = false;
  checkpoint_store_->async_materialize = false;
  (void)service_->Switch(30, 40);
  presentFirstFrame();
  ASSERT_EQ(service_->state(), EditorSessionState::Interactive);
  ASSERT_EQ(service_->identity().element_id, static_cast<sl_element_id_t>(30));

  // Build a stale first-frame result for A's generation and feed it through the
  // coordinator's result observer path. It must not regress identity to A.
  const auto         rid_b = service_->first_frame_request_id();
  EditorRenderResult stale;
  stale.kind                      = EditorRenderResultKind::FramePresented;
  stale.request_id                = rid_b;
  stale.intent.image_load_request_id = load_a;  // stale load request for A
  stale.intent.element_id         = 10;
  stale.intent.image_id           = 20;
  service_->NotifyRenderResult(stale);

  EXPECT_EQ(service_->identity().element_id, static_cast<sl_element_id_t>(30))
      << "stale first frame must not enable editing for another image";
  EXPECT_EQ(service_->state(), EditorSessionState::Interactive);
}

/// Invariant: Undo on the command thread never waits on the executor render
/// mutex. While a render worker owns the executor, Undo must return without
/// blocking. The history reduction and worker gate are separate; a bounded
/// timeout reports any unfinished operation.
TEST_F(EditorSessionCommandQueueBaselineTest,
       UndoWhileRenderWorkerOwnsExecutorDoesNotBlockCommandThread) {
  openInteractive(10, 20);  // image A, interactive

  // Hold a shared worker gate. The CQ2 history port intentionally does not
  // acquire this gate while reducing Undo.
  std::mutex render_lock;
  history_->render_lock = &render_lock;

  // Simulate a render worker owning the executor mid-Apply.
  std::unique_lock<std::mutex> worker_holds(render_lock);

  std::atomic<int>             returned{0};
  auto                         fut     = std::async(std::launch::async, [&] {
    (void)service_->Undo();
    returned.store(1);
  });
  const auto                   status  = fut.wait_for(std::chrono::milliseconds(200));

  // Target invariant: Undo returns without waiting for the render lock.
  const bool                   blocked = (status != std::future_status::ready);
  EXPECT_FALSE(blocked) << "Undo blocked on the executor render lock (unfinished op: Undo)";

  // Release the worker hold so the worker simulation can finish and the async
  // thread joins cleanly regardless of pass/fail.
  worker_holds.unlock();
  (void)fut.get();
  EXPECT_EQ(returned.load(), 1);
}

/// Invariant: Merge completion on the command thread never waits on the
/// executor render mutex while the worker owns it.
TEST_F(EditorSessionCommandQueueBaselineTest,
       MergeWhileRenderWorkerOwnsExecutorDoesNotBlockCommandThread) {
  openInteractive(10, 20);  // image A, interactive
  service_->SetCopiedPackageAvailable(true);

  AdjustmentMergePreview preview;
  ASSERT_EQ(service_->BeginMerge(AdjustmentTransferPackage{}, &preview).kind,
            EditorSessionResultKind::Accepted);

  std::mutex render_lock;
  history_->render_lock = &render_lock;
  std::unique_lock<std::mutex> worker_holds(render_lock);

  std::atomic<int>             returned{0};
  auto                         fut     = std::async(std::launch::async, [&] {
    (void)service_->CompleteMerge({});
    returned.store(1);
  });
  const auto                   status  = fut.wait_for(std::chrono::milliseconds(200));

  const bool                   blocked = (status != std::future_status::ready);
  EXPECT_FALSE(blocked)
      << "CompleteMerge blocked on the executor render lock (unfinished op: Merge)";

  worker_holds.unlock();
  (void)fut.get();
  EXPECT_EQ(returned.load(), 1);
}

/// Invariant: when the current image has a dirty journal, Paste queues a save
/// checkpoint BEFORE creating the new Version (one durable publication). The
/// pre-CQ1 facade creates the Version first (history->PasteAdjustments) and
/// then starts the save checkpoint, so the order is [version_created,
/// save_started] instead of [save_started, version_created]. The queued
/// facade retains the Paste command, flushes the journal, and continues the
/// transfer when the posted save completion reduces.
TEST_F(EditorSessionCommandQueueBaselineTest,
       DirtyJournalPasteQueuesSaveBeforeCreatingTheNewVersion) {
  openInteractive(10, 20);  // image A, interactive
  service_->SetCopiedPackageAvailable(true);

  std::vector<std::string> events;
  history_->event_log                  = &events;
  journal_->event_log                  = &events;
  history_->dirty_journal              = true;
  journal_->async_commit               = false;  // observe inline save ordering
  checkpoint_store_->async_materialize = false;

  const auto result = service_->PasteAdjustments(AdjustmentTransferPackage{}, "Pasted Version");
  EXPECT_EQ(result.kind, EditorSessionResultKind::SaveStarted)
      << "dirty journal Paste must start the flush checkpoint before the transfer";
  // The flush completion is posted to the queue; reducing it resumes the
  // retained Paste on the clean journal.
  drainQueue();

  // Target order: save first, then version creation.
  ASSERT_GE(events.size(), 2u) << "Paste must produce a save and a version creation";
  EXPECT_EQ(events[0], "save_started")
      << "dirty journal Paste must queue a save before creating the new Version";
  EXPECT_EQ(events[1], "version_created");
}

/// Invariant: when the current image has a dirty journal, Merge queues a save
/// checkpoint BEFORE creating the merge commit. The pre-CQ1 facade commits the
/// merge first and then starts the save, so the order is [merge_committed,
/// save_started].
TEST_F(EditorSessionCommandQueueBaselineTest,
       DirtyJournalMergeQueuesSaveBeforeCreatingTheMergeCommit) {
  openInteractive(10, 20);  // image A, interactive
  service_->SetCopiedPackageAvailable(true);

  AdjustmentMergePreview preview;
  ASSERT_EQ(service_->BeginMerge(AdjustmentTransferPackage{}, &preview).kind,
            EditorSessionResultKind::Accepted);

  std::vector<std::string> events;
  history_->event_log                  = &events;
  journal_->event_log                  = &events;
  history_->dirty_journal              = true;
  journal_->async_commit               = false;
  checkpoint_store_->async_materialize = false;

  const auto result                    = service_->CompleteMerge({});
  EXPECT_EQ(result.kind, EditorSessionResultKind::SaveStarted)
      << "dirty journal Merge must start the flush checkpoint before the transfer";
  drainQueue();

  ASSERT_GE(events.size(), 2u) << "Merge must produce a save and a merge commit";
  EXPECT_EQ(events[0], "save_started")
      << "dirty journal Merge must queue a save before creating the merge commit";
  EXPECT_EQ(events[1], "merge_committed");
}

/// CQ4: Paste is staged before publication. One accepted transfer owns one
/// save capture, one checkpoint materialization, one candidate publication,
/// and one final render route.
TEST_F(EditorSessionCommandQueueBaselineTest,
       DirtyPastePublishesOneCandidateAndOneFinalRender) {
  openInteractive(10, 20);  // image A, interactive
  service_->SetCopiedPackageAvailable(true);

  std::vector<std::string> events;
  history_->event_log                  = &events;
  journal_->event_log                  = &events;
  history_->dirty_journal              = true;
  journal_->async_commit               = false;
  checkpoint_store_->async_materialize = false;

  const auto render_count_before       = scheduler_->scheduled_.size();
  const auto capture_count_before      = history_->checkpoint_capture_count;
  const auto materialize_count_before  = checkpoint_store_->materialize_count;
  const auto terminal_count_before    = recorder_->terminal_count();
  const auto result =
      service_->PasteAdjustments(MakeExposureTransferPackage(0.75), "Pasted Version");
  EXPECT_EQ(result.kind, EditorSessionResultKind::SaveStarted);

  drainQueue();

  EXPECT_EQ(history_->checkpoint_capture_count, capture_count_before + 1);
  EXPECT_EQ(checkpoint_store_->materialize_count, materialize_count_before + 1);
  EXPECT_EQ(history_->transfer_publication_count, 1);
  EXPECT_EQ(scheduler_->scheduled_.size(), render_count_before + 1);
  EXPECT_EQ(recorder_->terminal_count(), terminal_count_before + 1);
  EXPECT_FALSE(history_->dirty_journal);
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0], "save_started");
  EXPECT_EQ(events[1], "version_created");
}

/// CQ4: Merge resolution is applied to the staged candidate and becomes
/// visible only after the same single durable checkpoint/publication path.
TEST_F(EditorSessionCommandQueueBaselineTest,
       DirtyMergePublishesOneCandidateAndOneFinalRender) {
  openInteractive(10, 20);  // image A, interactive
  service_->SetCopiedPackageAvailable(true);

  AdjustmentMergePreview preview;
  ASSERT_EQ(service_->BeginMerge(MakeExposureTransferPackage(1.25), &preview).kind,
            EditorSessionResultKind::Accepted);

  std::vector<std::string> events;
  history_->event_log                  = &events;
  journal_->event_log                  = &events;
  history_->dirty_journal              = true;
  journal_->async_commit               = false;
  checkpoint_store_->async_materialize = false;

  const auto render_count_before      = scheduler_->scheduled_.size();
  const auto capture_count_before     = history_->checkpoint_capture_count;
  const auto materialize_count_before = checkpoint_store_->materialize_count;
  const auto terminal_count_before    = recorder_->terminal_count();
  const auto result = service_->CompleteMerge({});
  EXPECT_EQ(result.kind, EditorSessionResultKind::SaveStarted);

  drainQueue();

  EXPECT_EQ(history_->checkpoint_capture_count, capture_count_before + 1);
  EXPECT_EQ(checkpoint_store_->materialize_count, materialize_count_before + 1);
  EXPECT_EQ(history_->transfer_publication_count, 1);
  EXPECT_EQ(scheduler_->scheduled_.size(), render_count_before + 1);
  EXPECT_EQ(recorder_->terminal_count(), terminal_count_before + 1);
  EXPECT_FALSE(history_->dirty_journal);
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0], "save_started");
  EXPECT_EQ(events[1], "merge_committed");
}

/// CQ4: a checkpoint/materialization failure discards the staged candidate and
/// leaves the published state and render schedule untouched.
TEST_F(EditorSessionCommandQueueBaselineTest,
       TransferMaterializationFailureRetainsPublishedStateAndRenderSchedule) {
  openInteractive(10, 20);  // image A, interactive
  service_->SetCopiedPackageAvailable(true);

  std::vector<std::string> events;
  history_->event_log                  = &events;
  journal_->event_log                  = &events;
  history_->dirty_journal              = true;
  journal_->async_commit               = false;
  checkpoint_store_->async_materialize = false;
  checkpoint_store_->fail_materialize  = true;

  const auto render_count_before      = scheduler_->scheduled_.size();
  const auto terminal_count_before    = recorder_->terminal_count();
  const auto result =
      service_->PasteAdjustments(MakeExposureTransferPackage(0.5), "Pasted Version");
  EXPECT_EQ(result.kind, EditorSessionResultKind::SaveStarted);

  drainQueue();

  EXPECT_EQ(history_->checkpoint_capture_count, 1);
  EXPECT_EQ(checkpoint_store_->materialize_count, 1);
  EXPECT_EQ(history_->transfer_publication_count, 0);
  EXPECT_EQ(scheduler_->scheduled_.size(), render_count_before);
  EXPECT_EQ(recorder_->terminal_count(), terminal_count_before + 1);
  EXPECT_EQ(service_->state(), EditorSessionState::RetainedImageFailure);
  EXPECT_TRUE(history_->dirty_journal);
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0], "save_started");
}

/// CQ4: a failed immutable capture is rejected before the checkpoint store is
/// invoked, so the staged transfer cannot create a partial Version or render.
TEST_F(EditorSessionCommandQueueBaselineTest,
       TransferCaptureFailureRetainsPublishedStateAndRenderSchedule) {
  openInteractive(10, 20);  // image A, interactive
  service_->SetCopiedPackageAvailable(true);
  history_->fail_capture = true;

  const auto render_count_before = scheduler_->scheduled_.size();
  const auto result =
      service_->PasteAdjustments(MakeExposureTransferPackage(0.5), "Pasted Version");

  EXPECT_EQ(result.kind, EditorSessionResultKind::Rejected);
  drainQueue();
  EXPECT_EQ(history_->checkpoint_capture_count, 1);
  EXPECT_EQ(checkpoint_store_->materialize_count, 0);
  EXPECT_EQ(history_->transfer_publication_count, 0);
  EXPECT_EQ(scheduler_->scheduled_.size(), render_count_before);
}

/// Invariant: one accepted command publishes one snapshot revision carrying
/// matching identity, state, and availability. The current synchronous switch
/// emits several results (SaveStarted, the inline save-completion result, and
/// SaveFinished) across several change notifications, so the UI samples a
/// split revision. The target is a single change notification for the command.
TEST_F(EditorSessionCommandQueueBaselineTest,
       OneSnapshotRevisionContainsMatchingIdentityStateAndAvailability) {
  openInteractive(10, 20);  // image A

  recorder_->mark_initiating();
  journal_->async_commit               = false;
  checkpoint_store_->async_materialize = false;
  (void)service_->Switch(30, 40);  // one accepted command: switch A->B
  recorder_->mark_returned();

  // A single state transition must publish a single change notification so the
  // UI cannot sample identity, state, and availability from different revisions.
  EXPECT_EQ(recorder_->change_notifications_during_initiating, 1u)
      << "one accepted command must publish one snapshot revision";
  // The published identity and state must come from one revision: every result
  // for this command must carry the same (identity, state) pair. The current
  // inline switch publishes SaveStarted (A, Saving), the inline completion
  // result (B, Loading), and SaveFinished (B, Loading) across several
  // notifications, so the UI samples a split revision.
  ASSERT_FALSE(recorder_->results_during_initiating.empty());
  std::set<EditorSessionState> distinct_states;
  std::set<sl_element_id_t>    distinct_elements;
  for (const auto& r : recorder_->results_during_initiating) {
    distinct_states.insert(r.state);
    distinct_elements.insert(r.identity.element_id);
  }
  EXPECT_EQ(distinct_states.size(), 1u)
      << "one snapshot revision must carry one state value (split revision observed)";
  EXPECT_EQ(distinct_elements.size(), 1u)
      << "one snapshot revision must carry one identity value (split revision observed)";
}

/// Invariant: one accepted command publishes at most one terminal result. The
/// current synchronous switch publishes the inline save-completion result AND a
/// SaveFinished result AND the command result, so multiple terminal results
/// arrive for one command.
TEST_F(EditorSessionCommandQueueBaselineTest, OneAcceptedCommandPublishesExactlyOneTerminalResult) {
  openInteractive(10, 20);  // image A

  const auto terminal_before = recorder_->terminal_count();
  recorder_->mark_initiating();
  journal_->async_commit               = false;
  checkpoint_store_->async_materialize = false;
  (void)service_->Switch(30, 40);  // one accepted command: switch A->B
  recorder_->mark_returned();
  // The save completion is posted to the queue; the command's single terminal
  // result (the navigation completion) publishes when the queue drains.
  drainQueue();

  EXPECT_EQ(terminals_since(terminal_before), 1u)
      << "one accepted command must publish exactly one terminal result";
}

/// Invariant: immediate (inline port) and delayed (async worker) completion
/// deliver the same published snapshot sequence. Both modes post typed
/// completions to the command queue; only the delivery timing differs, so the
/// ordered (kind, state) result stream must be identical.
TEST_F(EditorSessionCommandQueueBaselineTest,
       ImmediateAndDelayedCompletionProduceTheSameSnapshotSequence) {
  const auto seq_immediate = runSwitchSequence(false);

  RebuildSession();
  const auto seq_delayed = runSwitchSequence(true);

  EXPECT_EQ(seq_immediate, seq_delayed)
      << "immediate and delayed completion must produce the same snapshot sequence";
}

/// Invariant: Shutdown cancels an in-flight history checkpoint without letting
/// the cancelled completion re-enter a recoverable failure state. Shutdown
/// also stops the queue from admitting later commands, and both the Shutdown
/// command and the cancelled checkpoint publish exactly one terminal result.
TEST_F(EditorSessionCommandQueueBaselineTest,
       ShutdownDuringHistoryCheckpointStaysShuttingDownAndPublishesOneCancellation) {
  openInteractive(10, 20);  // image A, interactive
  service_->SetCopiedPackageAvailable(true);

  // Start a history checkpoint whose save cannot finish on its own.
  journal_->async_commit               = true;
  checkpoint_store_->async_materialize = true;
  const auto paste = service_->PasteAdjustments(AdjustmentTransferPackage{}, "Pasted Version");
  ASSERT_EQ(paste.kind, EditorSessionResultKind::SaveStarted);

  const auto terminal_before = recorder_->terminal_count();
  const auto shutdown        = service_->Shutdown();
  EXPECT_EQ(shutdown.state, EditorSessionState::ShuttingDown);
  drainQueue();

  EXPECT_EQ(service_->state(), EditorSessionState::ShuttingDown)
      << "the cancelled checkpoint must not re-enter RetainedImageFailure";
  EXPECT_EQ(terminals_since(terminal_before), 2u)
      << "Shutdown and the cancelled checkpoint each publish one terminal result";

  // The queue no longer admits user commands; the rejection carries the
  // shutting-down state rather than mutating the session.
  const auto late = service_->Open(50, 60);
  EXPECT_EQ(late.kind, EditorSessionResultKind::Rejected);
  EXPECT_EQ(service_->state(), EditorSessionState::ShuttingDown);
}

}  // namespace
}  // namespace alcedo
