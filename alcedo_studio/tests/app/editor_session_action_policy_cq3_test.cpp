//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file editor_session_action_policy_cq3_test.cpp
/// @brief CQ3 acceptance: command admission matches published availability,
///        operation leases, scoped request-id correlation, and QML API bans.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "app/adjustment_transfer_types.hpp"
#include "app/editor_action_policy.hpp"
#include "app/editor_session_bootstrap.hpp"
#include "app/editor_session_service.hpp"
#include "app/editor_session_types.hpp"
#include "support/editor_parameter_target_test.hpp"
#include "support/editor_session_command_queue_test_support.hpp"
#include "type/hash_type.hpp"

namespace alcedo {
namespace {
using namespace alcedo::test;

class RecordingScheduler final : public IEditorPipelineSchedulerPort {
 public:
  auto Schedule(const EditorRenderRequest& request,
                EditorPipelineScheduleCompletion /*on_complete*/ = {}) -> std::uint64_t override {
    scheduled_.push_back(request);
    return ++next_job_;
  }
  void Cancel(std::uint64_t job_id) override { cancelled_.push_back(job_id); }
  void WaitForSessionIdle(std::uint64_t) override {}

  std::vector<EditorRenderRequest> scheduled_;
  std::vector<std::uint64_t>       cancelled_;
  std::uint64_t                    next_job_ = 0;
};

class EditorSessionActionPolicyCq3Test : public ::testing::Test {
 protected:
  void SetUp() override {
    history_          = std::make_shared<ControllableEditorHistoryPort>();
    pipeline_         = std::make_shared<FakeEditorPipelinePort>();
    tasks_            = std::make_shared<FakeEditorTaskPort>();
    journal_          = std::make_shared<OrderRecordingJournalPort>();
    scheduler_        = std::make_shared<RecordingScheduler>();
    checkpoint_store_ = std::make_shared<FakeEditorCheckpointStore>();
    thumbnails_       = std::make_shared<FakeEditorThumbnailPort>();
    runtime_ = EditorSessionRuntime::CreateWithPorts(pipeline_, history_, tasks_, journal_,
                                                     scheduler_, checkpoint_store_, thumbnails_);
    service_ = runtime_->service.get();
    service_->SetPresentationSinkId(1);
    service_->SetPresentationSize(640, 480);
    service_->SetActionAvailabilityObserver(
        [this](const EditorActionAvailability& next) { availability_events_.push_back(next); });
  }

  void drainQueue() { service_->DrainCommandQueueForTests(); }

  void presentFirstFrame() {
    drainQueue();
    const auto rid = service_->first_frame_request_id();
    if (rid == 0) {
      return;
    }
    runtime_->coordinator->NotifySchedulerCompleted(rid, true);
    drainQueue();
    const auto quality_rid = runtime_->coordinator->last_scheduled_request_id();
    if (quality_rid != rid) {
      runtime_->coordinator->NotifySchedulerCompleted(quality_rid, true);
      drainQueue();
    }
  }

  void openInteractive(sl_element_id_t eid = 10, image_id_t iid = 20) {
    (void)service_->Open(eid, iid);
    presentFirstFrame();
    ASSERT_EQ(service_->state(), EditorSessionState::Interactive);
  }

  [[nodiscard]] auto Decision(EditorAction action) const -> EditorActionDecision {
    return service_->action_availability().For(action);
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
  std::vector<EditorActionAvailability>          availability_events_;
};

TEST_F(EditorSessionActionPolicyCq3Test,
       CommandAcceptanceMatchesPublishedAvailabilityForEveryEditorAction) {
  openInteractive();
  service_->SetCopiedPackageAvailable(true);
  history_->force_can_undo = false;
  history_->force_can_redo = false;
  (void)service_->RequestViewChange(EditorRenderReason::ZoomPan, std::nullopt);
  drainQueue();

  const auto published = service_->action_availability();
  EditorActionInputs inputs;
  inputs.session_state     = service_->state();
  inputs.has_image         = service_->has_image();
  inputs.package_available = true;
  inputs.can_undo          = false;
  inputs.can_redo          = false;
  const auto evaluated =
      EditorActionPolicy::EvaluateAll(EditorCommandContext{}, inputs);

  for (std::size_t i = 0; i < EditorActionCount(); ++i) {
    const auto action = static_cast<EditorAction>(i);
    EXPECT_EQ(published.For(action).allowed, evaluated.For(action).allowed)
        << EditorActionName(action);
  }

  // Denied actions are rejected by SubmitCommand with the same reason class.
  const auto undo = service_->Undo();
  EXPECT_EQ(undo.kind, EditorSessionResultKind::Rejected);
  EXPECT_FALSE(published.For(EditorAction::Undo).allowed);

  EXPECT_TRUE(published.For(EditorAction::SelectImage).allowed);
  EXPECT_TRUE(published.For(EditorAction::PreviewAdjustment).allowed);
  EXPECT_TRUE(published.For(EditorAction::ApplyPaste).allowed);
}

TEST(EditorSessionNodeCommandPolicy,
     RenameAndEditNodeGraphUseTheSameAdmissionDecisionAsSettledAdjustments) {
  EXPECT_EQ(EditorActionPolicy::ActionForCommand(EditorSessionCommandKind::RenameColorGrade),
            EditorAction::CommitAdjustment);
  EXPECT_EQ(EditorActionPolicy::ActionForCommand(EditorSessionCommandKind::EditNodeGraph),
            EditorAction::CommitAdjustment);
}

TEST_F(EditorSessionActionPolicyCq3Test,
       AcceptedOperationLeaseBlocksAndCompletionRestoresExactlyItsDeclaredActions) {
  openInteractive();
  service_->SetCopiedPackageAvailable(true);
  (void)service_->RequestViewChange(EditorRenderReason::ZoomPan, std::nullopt);
  drainQueue();
  ASSERT_TRUE(Decision(EditorAction::PreviewAdjustment).allowed);
  ASSERT_TRUE(Decision(EditorAction::ApplyPaste).allowed);

  journal_->async_commit               = true;
  checkpoint_store_->async_materialize = true;
  availability_events_.clear();
  const auto started = service_->Switch(30, 40);
  ASSERT_EQ(started.kind, EditorSessionResultKind::SaveStarted);
  drainQueue();

  EXPECT_FALSE(Decision(EditorAction::PreviewAdjustment).allowed);
  EXPECT_FALSE(Decision(EditorAction::ApplyPaste).allowed);
  EXPECT_FALSE(Decision(EditorAction::Undo).allowed);
  // SelectImage remains admissible so a rapid follow-up can queue.
  EXPECT_TRUE(Decision(EditorAction::SelectImage).allowed);

  journal_->CompleteCommit(true);
  checkpoint_store_->CompleteMaterialization(true);
  drainQueue();
  presentFirstFrame();

  EXPECT_EQ(service_->state(), EditorSessionState::Interactive);
  EXPECT_TRUE(Decision(EditorAction::PreviewAdjustment).allowed);
  EXPECT_TRUE(Decision(EditorAction::ApplyPaste).allowed);
}

TEST_F(EditorSessionActionPolicyCq3Test, SavingCheckpointRejectsSettledEditCheckoutAndPaste) {
  openInteractive();
  service_->SetCopiedPackageAvailable(true);
  journal_->async_commit               = true;
  checkpoint_store_->async_materialize = true;
  const auto started                   = service_->Switch(30, 40);
  ASSERT_EQ(started.kind, EditorSessionResultKind::SaveStarted);
  drainQueue();
  ASSERT_EQ(service_->state(), EditorSessionState::Saving);
  const int commits_before   = history_->commit_count;
  const int checkouts_before = history_->checkout_count;

  auto       patch = WithColorGradeTarget({"exposure", R"({"exposure_ev":2.0})", true});
  const auto edit  = service_->CommitAdjustment(patch);
  EXPECT_EQ(edit.kind, EditorSessionResultKind::Rejected);
  EXPECT_FALSE(Decision(EditorAction::CommitAdjustment).allowed);

  const auto checkout = service_->CheckoutVersion(Hash128{0x11ULL, 0x22ULL});
  EXPECT_EQ(checkout.kind, EditorSessionResultKind::Rejected);
  EXPECT_FALSE(Decision(EditorAction::CheckoutVersion).allowed);

  AdjustmentTransferPackage package;
  const auto                paste = service_->PasteAdjustments(package, "Pasted");
  EXPECT_EQ(paste.kind, EditorSessionResultKind::Rejected);
  EXPECT_FALSE(Decision(EditorAction::ApplyPaste).allowed);

  EXPECT_EQ(history_->commit_count, commits_before);
  EXPECT_EQ(history_->checkout_count, checkouts_before);
  EXPECT_EQ(service_->identity().element_id, 10u);

  journal_->CompleteCommit(true);
  checkpoint_store_->CompleteMaterialization(true);
  drainQueue();
  presentFirstFrame();
  EXPECT_EQ(service_->state(), EditorSessionState::Interactive);
  EXPECT_EQ(service_->identity().element_id, 30u);
  EXPECT_TRUE(Decision(EditorAction::CommitAdjustment).allowed);
  EXPECT_TRUE(Decision(EditorAction::CheckoutVersion).allowed);
}

TEST_F(EditorSessionActionPolicyCq3Test, RejectedCommandDoesNotChangeAvailability) {
  openInteractive();
  const auto before = service_->action_availability();
  availability_events_.clear();
  const auto rejected = service_->Undo();
  EXPECT_EQ(rejected.kind, EditorSessionResultKind::Rejected);
  EXPECT_EQ(service_->action_availability(), before);
  EXPECT_TRUE(availability_events_.empty());
}

TEST_F(EditorSessionActionPolicyCq3Test, StaleImageLoadRequestCannotAcquireTheCurrentImage) {
  openInteractive(10, 20);
  const auto current = service_->active_image_load_request();
  ASSERT_TRUE(current.valid());
  service_->NotifyImageAcquired(ImageLoadRequestId{current.value + 99}, true, "stale");
  drainQueue();
  EXPECT_EQ(service_->identity().element_id, 10u);
  EXPECT_EQ(service_->identity().image_id, 20u);
  EXPECT_EQ(service_->state(), EditorSessionState::Interactive);
}

TEST_F(EditorSessionActionPolicyCq3Test, StaleRenderRequestCannotPresentAFrameOrEnableEditing) {
  openInteractive(10, 20);
  ASSERT_EQ(service_->state(), EditorSessionState::Interactive);

  EditorRenderResult stale;
  stale.kind       = EditorRenderResultKind::FrameReady;
  stale.request_id = 999999;
  stale.intent.element_id            = 10;
  stale.intent.image_id              = 20;
  stale.intent.image_load_request_id = ImageLoadRequestId{999};
  service_->NotifyRenderResult(stale);
  drainQueue();

  EXPECT_EQ(service_->state(), EditorSessionState::Interactive);
  EXPECT_EQ(service_->identity().element_id, 10u);
}

TEST_F(EditorSessionActionPolicyCq3Test,
       ImageAtoBtoARejectsTheFirstARenderWithoutSessionGeneration) {
  openInteractive(10, 20);  // A
  const auto a_load = service_->active_image_load_request();
  ASSERT_TRUE(a_load.valid());

  journal_->async_commit               = false;
  checkpoint_store_->async_materialize = false;
  (void)service_->Switch(30, 40);  // B
  drainQueue();
  presentFirstFrame();
  ASSERT_EQ(service_->identity().image_id, 40u);
  const auto b_load = service_->active_image_load_request();
  ASSERT_NE(a_load, b_load);

  (void)service_->Switch(10, 20);  // back to A
  drainQueue();
  presentFirstFrame();
  ASSERT_EQ(service_->identity().image_id, 20u);
  const auto a2_load = service_->active_image_load_request();
  ASSERT_NE(a_load, a2_load);

  // A stale first-A render request must not change identity or state.
  EditorRenderResult stale_a;
  stale_a.kind                       = EditorRenderResultKind::FrameReady;
  stale_a.request_id                 = 1;
  stale_a.intent.element_id          = 10;
  stale_a.intent.image_id            = 20;
  stale_a.intent.image_load_request_id = a_load;
  service_->NotifyRenderResult(stale_a);
  drainQueue();
  EXPECT_EQ(service_->active_image_load_request(), a2_load);
  EXPECT_EQ(service_->identity().image_id, 20u);
  EXPECT_EQ(service_->state(), EditorSessionState::Interactive);

  // Public identity must not carry generation fields.
  const auto id = service_->identity();
  EXPECT_EQ(id.element_id, 10u);
  EXPECT_EQ(id.image_id, 20u);
}

TEST_F(EditorSessionActionPolicyCq3Test,
       AvailabilityPublishesAtMostOncePerCommandOrCompletionReduction) {
  openInteractive();
  availability_events_.clear();
  (void)service_->RequestViewChange(EditorRenderReason::ZoomPan, std::nullopt);
  drainQueue();
  EXPECT_LE(availability_events_.size(), 1u);

  availability_events_.clear();
  journal_->async_commit               = true;
  checkpoint_store_->async_materialize = true;
  (void)service_->Switch(30, 40);
  EXPECT_LE(availability_events_.size(), 1u);
  journal_->CompleteCommit(true);
  checkpoint_store_->CompleteMaterialization(true);
  drainQueue();
  EXPECT_LE(availability_events_.size(), 2u);
}

TEST_F(EditorSessionActionPolicyCq3Test,
       BackgroundRestrictionAndHistoryFactsUseTheSameDecisionFunction) {
  openInteractive();
  history_->force_can_undo = true;
  history_->force_can_redo = true;
  service_->SetCopiedPackageAvailable(true);

  EditorBackgroundActionRestrictions blocked;
  blocked.blocks_paste = true;
  service_->SetBackgroundActionRestrictions(blocked);
  (void)service_->RequestViewChange(EditorRenderReason::ZoomPan, std::nullopt);
  drainQueue();

  EditorActionInputs inputs;
  inputs.session_state           = EditorSessionState::Interactive;
  inputs.has_image               = true;
  inputs.can_undo                = true;
  inputs.can_redo                = true;
  inputs.package_available       = true;
  inputs.background_blocks_paste = true;

  const auto paste = EditorActionPolicy::Evaluate(EditorAction::ApplyPaste, {}, inputs);
  const auto undo  = EditorActionPolicy::Evaluate(EditorAction::Undo, {}, inputs);
  EXPECT_FALSE(paste.allowed);
  EXPECT_TRUE(undo.allowed);
  EXPECT_EQ(Decision(EditorAction::ApplyPaste).allowed, paste.allowed);
}

TEST_F(EditorSessionActionPolicyCq3Test,
       AdjustmentPanelsReloadOnlyWhenCommittedContentChanges) {
  // Pure policy: interactive preview must not be modeled as a committed
  // content change. The controller emits AdjustmentSnapshotChanged only when
  // BuildSnapshotMap content changes; this test pins the service side that
  // interactive Patch does not bump history_revision.
  openInteractive();
  const auto before = service_->history_revision();
  auto patch = WithColorGradeTarget({"exposure", R"({"ev":0.5})", false});
  (void)service_->Patch(patch);
  drainQueue();
  EXPECT_EQ(service_->history_revision(), before);

  (void)service_->CommitAdjustment(patch);
  drainQueue();
  EXPECT_GT(service_->history_revision(), before);
}

TEST(EditorSessionActionPolicyStaticApiBan, QmlAndPublicApiOmitSessionGenerationAndSnapshotRevision) {
  namespace fs = std::filesystem;
  const fs::path roots[] = {
      fs::path("alcedo_studio/src/ui/alcedo_main/qml"),
      fs::path("alcedo_studio/src/include/ui/alcedo_main/album_backend"),
      fs::path("alcedo_studio/src/include/app"),
  };
  const std::set<std::string> banned = {"sessionGeneration", "snapshotRevision"};
  std::vector<std::string>    hits;
  for (const auto& root : roots) {
    if (!fs::exists(root)) {
      continue;
    }
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
      if (!entry.is_regular_file()) {
        continue;
      }
      const auto ext = entry.path().extension().string();
      if (ext != ".qml" && ext != ".hpp" && ext != ".cpp") {
        continue;
      }
      // Internal controller helpers may still keep a private mirror for
      // viewport stamping; ban only public Q_PROPERTY / QML identifiers.
      std::ifstream in(entry.path());
      std::string   line;
      while (std::getline(in, line)) {
        if (line.find("Q_PROPERTY") == std::string::npos && ext != ".qml") {
          continue;
        }
        for (const auto& token : banned) {
          if (line.find(token) != std::string::npos) {
            hits.push_back(entry.path().string() + ": " + line);
          }
        }
      }
    }
  }
  EXPECT_TRUE(hits.empty()) << "Banned public API tokens found:\n"
                            << [&] {
                                 std::string joined;
                                 for (const auto& h : hits) {
                                   joined += h + "\n";
                                 }
                                 return joined;
                               }();
}

}  // namespace
}  // namespace alcedo
