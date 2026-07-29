//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <algorithm>

#include "support/editor_session_navigation_fixture.hpp"

namespace alcedo {
namespace {

class EditorSessionNavigationControllerTest : public ::testing::Test {
 protected:
  void                                 SetUp() override { fixture_.SetUp(); }
  void                                 TearDown() override { fixture_.TearDown(); }

  test::EditorSessionNavigationFixture fixture_;
};

TEST_F(EditorSessionNavigationControllerTest, OpenWithNoPriorImageCompletesSynchronously) {
  const auto result = fixture_.nav().RequestOpenOrSwitch(10, 20, false);
  EXPECT_TRUE(result.completed_synchronously);
  EXPECT_FALSE(result.waiting_for_checkpoint);
  EXPECT_FALSE(result.rejected);
  EXPECT_FALSE(result.failed);
  EXPECT_TRUE(fixture_.lifecycle().has_image());
  EXPECT_EQ(fixture_.lifecycle().identity().element_id, static_cast<sl_element_id_t>(10));
  EXPECT_EQ(fixture_.lifecycle().identity().image_id, static_cast<image_id_t>(20));
}

/// Phase 4B: A→B waits for journal commit, materialize/truncate, and thumbnail
/// refresh before releasing A and acquiring B.
TEST_F(EditorSessionNavigationControllerTest,
       SwitchToBWaitsForACommitTruncateAndThumbnailCompletion) {
  fixture_.OpenA();

  const auto result = fixture_.RequestSwitchToB();
  EXPECT_TRUE(result.waiting_for_checkpoint);
  EXPECT_FALSE(result.completed_synchronously);
  EXPECT_TRUE(fixture_.nav().has_pending_action());
  EXPECT_TRUE(result.ticket.valid());
  ASSERT_GE(fixture_.events().size(), 2u);
  EXPECT_EQ(fixture_.events()[0], "checkpoint_a");
  EXPECT_EQ(fixture_.events()[1], "commit");
  // Materialize/truncate has not started until journal durability completes.
  EXPECT_EQ(std::count(fixture_.events().begin(), fixture_.events().end(), "truncate"), 0);
  EXPECT_EQ(std::count(fixture_.events().begin(), fixture_.events().end(), "thumbnail"), 0);
  EXPECT_EQ(std::count(fixture_.events().begin(), fixture_.events().end(), "acquire_b"), 0);

  fixture_.CompleteCheckpoint();
  EXPECT_FALSE(fixture_.nav().has_pending_action());
  EXPECT_TRUE(fixture_.lifecycle().has_image());
  EXPECT_EQ(fixture_.lifecycle().identity().element_id,
            test::EditorSessionNavigationFixture::kElementB);

  ASSERT_EQ(fixture_.events().size(), 6u);
  EXPECT_EQ(fixture_.events()[0], "checkpoint_a");
  EXPECT_EQ(fixture_.events()[1], "commit");
  EXPECT_EQ(fixture_.events()[2], "truncate");
  EXPECT_EQ(fixture_.events()[3], "thumbnail");
  EXPECT_EQ(fixture_.events()[4], "release_a");
  EXPECT_EQ(fixture_.events()[5], "acquire_b");
  EXPECT_EQ(fixture_.thumbnails().refresh_count, 1);
  EXPECT_EQ(fixture_.thumbnails().refreshed_ids.front(),
            test::EditorSessionNavigationFixture::kElementA);
}

TEST_F(EditorSessionNavigationControllerTest, CheckpointFailureKeepsAAndNeverAcquiresB) {
  fixture_.OpenA();

  const auto result = fixture_.RequestSwitchToB();
  EXPECT_TRUE(result.waiting_for_checkpoint);

  fixture_.FailCheckpoint("A materialization failed");
  EXPECT_FALSE(fixture_.nav().has_pending_action());
  EXPECT_EQ(fixture_.lifecycle().state(), EditorSessionState::RetainedImageFailure);
  EXPECT_TRUE(fixture_.lifecycle().has_image());
  EXPECT_TRUE(fixture_.nav().has_pending_recovery());
  EXPECT_EQ(fixture_.lifecycle().identity().element_id,
            test::EditorSessionNavigationFixture::kElementA);
  EXPECT_EQ(std::count(fixture_.events().begin(), fixture_.events().end(), "acquire_b"), 0);
  EXPECT_EQ(std::count(fixture_.events().begin(), fixture_.events().end(), "release_a"), 0);
  EXPECT_EQ(std::count(fixture_.events().begin(), fixture_.events().end(), "thumbnail"), 0);
  EXPECT_EQ(fixture_.thumbnails().refresh_count, 0);
  EXPECT_EQ(fixture_.pipeline().acquire_count, 1);
}

/// Phase 6C-6: Version checkout always completes a save checkpoint first, then
/// rebuilds on the same image without releasing A or acquiring B.
TEST_F(EditorSessionNavigationControllerTest,
       CheckoutVersionSavesFirstThenRebuildsWithoutReleasingImage) {
  fixture_.OpenA();
  const auto target_version   = Hash128{0xabcdef01ULL, 0x23456789ULL};
  const auto prior_render_gen = fixture_.lifecycle().identity().render_generation;

  const auto result           = fixture_.RequestCheckoutVersion(target_version);
  EXPECT_TRUE(result.waiting_for_checkpoint);
  EXPECT_TRUE(fixture_.nav().has_pending_action());
  EXPECT_EQ(std::count(fixture_.events().begin(), fixture_.events().end(), "checkout_version"), 0);
  EXPECT_EQ(std::count(fixture_.events().begin(), fixture_.events().end(), "release_a"), 0);

  fixture_.CompleteCheckpoint();
  EXPECT_FALSE(fixture_.nav().has_pending_action());
  EXPECT_EQ(fixture_.lifecycle().identity().element_id,
            test::EditorSessionNavigationFixture::kElementA);
  EXPECT_EQ(fixture_.lifecycle().state(), EditorSessionState::Interactive);
  EXPECT_EQ(std::count(fixture_.events().begin(), fixture_.events().end(), "release_a"), 0);
  EXPECT_EQ(std::count(fixture_.events().begin(), fixture_.events().end(), "acquire_b"), 0);
  EXPECT_EQ(std::count(fixture_.events().begin(), fixture_.events().end(), "checkout_version"), 1);
  EXPECT_EQ(fixture_.history().checkout_count, 1);
  EXPECT_EQ(fixture_.history().last_checkout_version, target_version);
  EXPECT_EQ(fixture_.history().release_count, 0);
  // Same-image Version checkout must advance render generation so the
  // presentation path cannot keep the prior Version's frame without a re-route.
  EXPECT_GT(fixture_.lifecycle().identity().render_generation, prior_render_gen);
}

/// Phase 7A P0: failed checkout rebuild keeps the image and does not expose a
/// partial pipeline (history port fails closed; navigation retains recovery).
TEST_F(EditorSessionNavigationControllerTest,
       FailedVersionCheckoutKeepsHasImageAndLastFrameVisible) {
  fixture_.OpenA();
  fixture_.history().fail_checkout = true;
  const auto target_version        = Hash128{0x11111111ULL, 0x22222222ULL};

  const auto result                = fixture_.RequestCheckoutVersion(target_version);
  EXPECT_TRUE(result.waiting_for_checkpoint);
  fixture_.CompleteCheckpoint();

  EXPECT_FALSE(fixture_.nav().has_pending_action());
  EXPECT_EQ(fixture_.lifecycle().state(), EditorSessionState::RetainedImageFailure);
  EXPECT_TRUE(fixture_.lifecycle().has_image());
  EXPECT_TRUE(fixture_.nav().has_pending_recovery());
  EXPECT_EQ(fixture_.lifecycle().identity().element_id,
            test::EditorSessionNavigationFixture::kElementA);
  EXPECT_EQ(std::count(fixture_.events().begin(), fixture_.events().end(), "release_a"), 0);
  EXPECT_EQ(std::count(fixture_.events().begin(), fixture_.events().end(), "acquire_b"), 0);
  EXPECT_EQ(fixture_.history().checkout_count, 1);
}

TEST_F(EditorSessionNavigationControllerTest, SaveFailureRetryThenSwitchAcquiresRequestedImage) {
  fixture_.OpenA();
  const auto first = fixture_.RequestSwitchToB();
  ASSERT_TRUE(first.waiting_for_checkpoint);

  fixture_.FailCheckpoint("A materialization failed");
  ASSERT_EQ(fixture_.lifecycle().state(), EditorSessionState::RetainedImageFailure);
  ASSERT_TRUE(fixture_.nav().has_pending_recovery());

  const auto blocked = fixture_.nav().RequestOpenOrSwitch(5, 6, true);
  EXPECT_TRUE(blocked.rejected);
  EXPECT_EQ(fixture_.lifecycle().identity().element_id,
            test::EditorSessionNavigationFixture::kElementA);

  const auto retry = fixture_.nav().RetrySaveAfterFailure();
  EXPECT_TRUE(retry.waiting_for_checkpoint);
  EXPECT_FALSE(fixture_.nav().has_pending_recovery());
  fixture_.CompleteCheckpoint();
  fixture_.lifecycle().MarkFirstFramePresented();

  EXPECT_EQ(fixture_.lifecycle().state(), EditorSessionState::Interactive);
  EXPECT_TRUE(fixture_.lifecycle().has_image());
  EXPECT_EQ(fixture_.lifecycle().identity().element_id,
            test::EditorSessionNavigationFixture::kElementB);
  EXPECT_EQ(fixture_.lifecycle().identity().image_id,
            test::EditorSessionNavigationFixture::kImageB);
  EXPECT_FALSE(fixture_.nav().has_pending_action());
  EXPECT_FALSE(fixture_.nav().has_pending_recovery());
}

TEST_F(EditorSessionNavigationControllerTest,
       SaveFailureDiscardThenSwitchClearsJournalAndAcquiresRequestedImage) {
  fixture_.OpenA();
  ASSERT_TRUE(fixture_.RequestSwitchToB().waiting_for_checkpoint);
  fixture_.FailCheckpoint("A materialization failed");
  ASSERT_TRUE(fixture_.nav().has_pending_recovery());

  const auto discard = fixture_.nav().DiscardAndContinueAfterFailure();
  EXPECT_TRUE(discard.completed_synchronously);
  fixture_.lifecycle().MarkFirstFramePresented();
  EXPECT_EQ(fixture_.journal().discard_count, 1);
  EXPECT_EQ(fixture_.lifecycle().state(), EditorSessionState::Interactive);
  EXPECT_TRUE(fixture_.lifecycle().has_image());
  EXPECT_EQ(fixture_.lifecycle().identity().element_id,
            test::EditorSessionNavigationFixture::kElementB);
  EXPECT_FALSE(fixture_.nav().has_pending_recovery());
  EXPECT_FALSE(fixture_.nav().has_pending_action());
}

TEST_F(EditorSessionNavigationControllerTest,
       CreateRootVersionChecksOutRootAndNextCommitBelongsToNewVersion) {
  fixture_.OpenA();
  fixture_.journal().async_commit               = true;
  fixture_.checkpoint_store().async_materialize = true;
  const auto prior_render_gen                   = fixture_.lifecycle().identity().render_generation;

  const auto result = fixture_.nav().RequestCreateRootVersion("Root Look");
  ASSERT_TRUE(result.waiting_for_checkpoint);
  fixture_.CompleteCheckpoint();

  EXPECT_EQ(fixture_.history().root_version_count, 1);
  EXPECT_EQ(fixture_.history().active_version_id, fixture_.history().last_root_version);
  EXPECT_EQ(fixture_.lifecycle().state(), EditorSessionState::Interactive);
  EXPECT_TRUE(fixture_.lifecycle().has_image());
  // Post-create checkout must advance render generation so the viewport
  // cannot reuse the prior head's frame without a re-route.
  EXPECT_GT(fixture_.lifecycle().identity().render_generation, prior_render_gen);

  EditorAdjustmentPatch patch;
  patch.field_key = "exposure";
  std::string error;
  ASSERT_TRUE(
      fixture_.history().CommitAdjustment(fixture_.lifecycle().history_guard(), patch, &error))
      << error;
  EXPECT_EQ(fixture_.history().last_commit_version, fixture_.history().last_root_version);
}

TEST_F(EditorSessionNavigationControllerTest,
       BranchFromSelectedCommitChecksOutNamedRefWithoutDetachedHead) {
  fixture_.OpenA();
  fixture_.journal().async_commit               = true;
  fixture_.checkpoint_store().async_materialize = true;
  const auto target_commit                      = commit_hash_t{0x12345678ULL, 0x90abcdefULL};

  const auto result = fixture_.nav().RequestBranchFromCommit(target_commit, "Branch Look");
  ASSERT_TRUE(result.waiting_for_checkpoint);
  fixture_.CompleteCheckpoint();

  EXPECT_EQ(fixture_.history().branch_version_count, 1);
  EXPECT_EQ(fixture_.history().last_branch_commit, target_commit);
  EXPECT_EQ(fixture_.history().active_version_id, fixture_.history().last_branch_version);
  EXPECT_EQ(fixture_.history().last_commit_version, version_ref_id_t{});
  EXPECT_EQ(fixture_.lifecycle().state(), EditorSessionState::Interactive);
  EXPECT_TRUE(fixture_.lifecycle().has_image());
}

TEST_F(EditorSessionNavigationControllerTest,
       CreateOrBranchFailureRestoresPriorRefPipelineSnapshotAndFrame) {
  fixture_.OpenA();
  const auto prior_identity                     = fixture_.lifecycle().identity();
  const auto prior_version                      = fixture_.history().active_version_id;
  fixture_.history().fail_root_version          = true;
  fixture_.journal().async_commit               = true;
  fixture_.checkpoint_store().async_materialize = true;

  ASSERT_TRUE(fixture_.nav().RequestCreateRootVersion("Broken Root").waiting_for_checkpoint);
  fixture_.CompleteCheckpoint();
  EXPECT_EQ(fixture_.lifecycle().state(), EditorSessionState::RetainedImageFailure);
  EXPECT_TRUE(fixture_.lifecycle().has_image());
  EXPECT_TRUE(fixture_.nav().has_pending_recovery());
  EXPECT_EQ(fixture_.lifecycle().identity().element_id, prior_identity.element_id);
  EXPECT_EQ(fixture_.lifecycle().identity().image_id, prior_identity.image_id);
  EXPECT_EQ(fixture_.lifecycle().identity().session_generation, prior_identity.session_generation);
  EXPECT_EQ(fixture_.history().active_version_id, prior_version);
  EXPECT_EQ(std::count(fixture_.events().begin(), fixture_.events().end(), "release_a"), 0);

  fixture_.nav().CancelPendingNavigation();
  fixture_.history().fail_root_version   = false;
  fixture_.history().fail_branch_version = true;
  const auto target_commit               = commit_hash_t{0x22222222ULL, 0x33333333ULL};
  ASSERT_TRUE(fixture_.nav()
                  .RequestBranchFromCommit(target_commit, "Broken Branch")
                  .waiting_for_checkpoint);
  fixture_.CompleteCheckpoint();

  EXPECT_EQ(fixture_.lifecycle().state(), EditorSessionState::RetainedImageFailure);
  EXPECT_TRUE(fixture_.lifecycle().has_image());
  EXPECT_TRUE(fixture_.nav().has_pending_recovery());
  EXPECT_EQ(fixture_.lifecycle().identity().element_id, prior_identity.element_id);
  EXPECT_EQ(fixture_.lifecycle().identity().image_id, prior_identity.image_id);
  EXPECT_EQ(fixture_.lifecycle().identity().session_generation, prior_identity.session_generation);
  EXPECT_EQ(fixture_.history().active_version_id, prior_version);
  EXPECT_EQ(fixture_.history().last_branch_commit, target_commit);
  EXPECT_EQ(std::count(fixture_.events().begin(), fixture_.events().end(), "release_a"), 0);
}

TEST_F(EditorSessionNavigationControllerTest, SecondActionDoesNotReplaceOriginalTargetB) {
  fixture_.OpenA();

  const auto first = fixture_.RequestSwitchToB();
  EXPECT_TRUE(first.waiting_for_checkpoint);
  EXPECT_TRUE(fixture_.nav().has_pending_action());

  // A rapid second SwitchImage queues behind the running switch; it does not
  // replace B's running save target and is not rejected.
  const auto second = fixture_.nav().RequestOpenOrSwitch(5, 6, true);
  EXPECT_FALSE(second.rejected);
  EXPECT_TRUE(second.waiting_for_checkpoint);
  EXPECT_TRUE(fixture_.nav().has_pending_action());

  fixture_.CompleteCheckpoint();
  EXPECT_EQ(fixture_.lifecycle().identity().element_id,
            test::EditorSessionNavigationFixture::kElementB);
  EXPECT_NE(fixture_.lifecycle().identity().element_id, static_cast<sl_element_id_t>(5));
}

TEST_F(EditorSessionNavigationControllerTest, CloseWithNoPriorImageCompletesSynchronously) {
  const auto result = fixture_.nav().RequestClose(true);
  EXPECT_TRUE(result.completed_synchronously);
  EXPECT_FALSE(result.waiting_for_checkpoint);
  EXPECT_TRUE(fixture_.lifecycle().state() == EditorSessionState::NoImage ||
              fixture_.lifecycle().state() == EditorSessionState::ShuttingDown);
}

TEST_F(EditorSessionNavigationControllerTest, CloseWaitsForSaveThenCompletes) {
  fixture_.OpenA();

  fixture_.journal().async_commit               = true;
  fixture_.checkpoint_store().async_materialize = true;
  const auto result                             = fixture_.nav().RequestClose(true);
  EXPECT_TRUE(result.waiting_for_checkpoint);
  EXPECT_TRUE(fixture_.nav().has_pending_action());

  fixture_.CompleteCheckpoint();
  EXPECT_FALSE(fixture_.nav().has_pending_action());
}

TEST_F(EditorSessionNavigationControllerTest, ShutDownRejectsFurtherOpens) {
  fixture_.OpenA();
  fixture_.lifecycle().BeginShutdown();
  const auto result = fixture_.nav().RequestOpenOrSwitch(5, 6, false);
  EXPECT_TRUE(result.rejected);
}

TEST_F(EditorSessionNavigationControllerTest, SameImageIsNoop) {
  fixture_.OpenA();
  const auto identity = fixture_.lifecycle().identity();
  const auto result =
      fixture_.nav().RequestOpenOrSwitch(identity.element_id, identity.image_id, false);
  EXPECT_TRUE(result.completed_synchronously);
  EXPECT_TRUE(result.same_image_noop);
}

TEST_F(EditorSessionNavigationControllerTest, SaveCompletionDeliversThroughQueueNotInline) {
  fixture_.OpenA();
  const auto gen_a                = fixture_.lifecycle().identity().session_generation;

  // Inline (synchronous) save ports: the completion must still be posted to
  // the command executor and reduced only when Drain runs, never inline inside
  // SealAndStartSave.
  fixture_.journal().async_commit = false;
  fixture_.checkpoint_store().async_materialize = false;
  const auto result =
      fixture_.nav().RequestOpenOrSwitch(test::EditorSessionNavigationFixture::kElementB,
                                         test::EditorSessionNavigationFixture::kImageB, true);
  EXPECT_TRUE(result.waiting_for_checkpoint);
  EXPECT_TRUE(fixture_.nav().has_pending_action());
  // The inline save finished durable work, but navigation has not acquired B.
  EXPECT_EQ(fixture_.lifecycle().identity().element_id,
            test::EditorSessionNavigationFixture::kElementA);

  fixture_.Drain();
  EXPECT_FALSE(fixture_.nav().has_pending_action());
  EXPECT_TRUE(fixture_.lifecycle().has_image());
  EXPECT_EQ(fixture_.lifecycle().identity().element_id,
            test::EditorSessionNavigationFixture::kElementB);
  EXPECT_EQ(fixture_.lifecycle().identity().image_id,
            test::EditorSessionNavigationFixture::kImageB);
  EXPECT_EQ(fixture_.lifecycle().identity().session_generation, gen_a + 1);
  EXPECT_EQ(fixture_.pipeline().acquire_count, 2);
  EXPECT_EQ(fixture_.history().acquire_count, 2);
}

TEST_F(EditorSessionNavigationControllerTest, SaveFailureRetainsImageAfterQueuedCompletion) {
  fixture_.journal().fail_barrier = true;
  fixture_.OpenA();
  const auto gen_a                = fixture_.lifecycle().identity().session_generation;

  fixture_.journal().async_commit = false;
  fixture_.checkpoint_store().async_materialize = false;
  const auto result =
      fixture_.nav().RequestOpenOrSwitch(test::EditorSessionNavigationFixture::kElementB,
                                         test::EditorSessionNavigationFixture::kImageB, true);
  EXPECT_TRUE(result.waiting_for_checkpoint);
  EXPECT_TRUE(fixture_.nav().has_pending_action());

  fixture_.Drain();
  EXPECT_FALSE(fixture_.nav().has_pending_action());
  EXPECT_EQ(fixture_.lifecycle().identity().session_generation, gen_a);
  EXPECT_EQ(fixture_.lifecycle().state(), EditorSessionState::RetainedImageFailure);
  EXPECT_TRUE(fixture_.lifecycle().has_image());
  EXPECT_TRUE(fixture_.nav().has_pending_recovery());
}

TEST_F(EditorSessionNavigationControllerTest, CloseCompletionDeliversThroughQueueNotInline) {
  fixture_.OpenA();
  fixture_.journal().async_commit               = false;
  fixture_.checkpoint_store().async_materialize = false;
  const auto result                             = fixture_.nav().RequestClose(true);
  EXPECT_TRUE(result.waiting_for_checkpoint);
  EXPECT_TRUE(fixture_.nav().has_pending_action());

  fixture_.Drain();
  EXPECT_FALSE(fixture_.nav().has_pending_action());
  EXPECT_TRUE(fixture_.lifecycle().state() == EditorSessionState::NoImage ||
              fixture_.lifecycle().state() == EditorSessionState::ShuttingDown);
}

TEST_F(EditorSessionNavigationControllerTest, StaleCompletionWithWrongRequestIdKeepsPendingAction) {
  fixture_.OpenA();

  const auto result = fixture_.RequestSwitchToB();
  EXPECT_TRUE(result.waiting_for_checkpoint);
  EXPECT_TRUE(fixture_.nav().has_pending_action());

  SaveCheckpointResult stale;
  stale.request_id           = 999;
  stale.session_generation   = fixture_.lifecycle().identity().session_generation;
  stale.checkpoint_completed = true;
  fixture_.nav().OnCheckpointFinished(stale);

  EXPECT_TRUE(fixture_.nav().has_pending_action());
  EXPECT_EQ(fixture_.lifecycle().state(), EditorSessionState::Saving);

  fixture_.CompleteCheckpoint();
  EXPECT_FALSE(fixture_.nav().has_pending_action());
  EXPECT_EQ(fixture_.lifecycle().identity().element_id,
            test::EditorSessionNavigationFixture::kElementB);
}

TEST_F(EditorSessionNavigationControllerTest,
       StaleCompletionWithWrongSessionGenerationKeepsPendingAction) {
  fixture_.OpenA();

  const auto result = fixture_.RequestSwitchToB();
  EXPECT_TRUE(result.waiting_for_checkpoint);

  SaveCheckpointResult stale;
  stale.request_id           = 1;
  stale.session_generation   = 99;
  stale.checkpoint_completed = true;
  fixture_.nav().OnCheckpointFinished(stale);

  EXPECT_TRUE(fixture_.nav().has_pending_action());
  EXPECT_EQ(fixture_.lifecycle().state(), EditorSessionState::Saving);
}

/// Phase 4B: duplicate/stale completions must not resume B or finish the task twice.
TEST_F(EditorSessionNavigationControllerTest,
       DuplicateOrStaleCompletionCannotResumeBOrFinishTaskTwice) {
  fixture_.OpenA();
  const auto switch_result = fixture_.RequestSwitchToB();
  ASSERT_TRUE(switch_result.waiting_for_checkpoint);
  ASSERT_TRUE(switch_result.ticket.valid());

  const int ends_before = fixture_.tasks().end_count;
  fixture_.CompleteCheckpoint();
  EXPECT_EQ(fixture_.lifecycle().identity().element_id,
            test::EditorSessionNavigationFixture::kElementB);
  EXPECT_EQ(fixture_.tasks().end_count, ends_before + 1);
  const auto           b_generation = fixture_.lifecycle().identity().session_generation;

  // Stale success for the completed request must not re-acquire or re-finish.
  SaveCheckpointResult stale;
  stale.request_id           = switch_result.ticket.request_id;
  stale.session_generation   = switch_result.ticket.session_generation;
  stale.checkpoint_completed = true;
  fixture_.nav().OnCheckpointFinished(stale);
  fixture_.save_service().OnCheckpointFinished(stale);

  EXPECT_EQ(fixture_.lifecycle().identity().element_id,
            test::EditorSessionNavigationFixture::kElementB);
  EXPECT_EQ(fixture_.lifecycle().identity().session_generation, b_generation);
  EXPECT_EQ(fixture_.tasks().end_count, ends_before + 1);
  EXPECT_EQ(fixture_.pipeline().acquire_count, 2);
  EXPECT_EQ(std::count(fixture_.events().begin(), fixture_.events().end(), "acquire_b"), 1);
}

/// Phase 4B: close waits for its checkpoint; CancelAndWait publishes one terminal.
TEST_F(EditorSessionNavigationControllerTest, CloseAndShutdownEachProduceOneTerminalResult) {
  // Close path: one terminal after commit+materialize.
  fixture_.OpenA();
  int  close_terminals                          = 0;
  bool close_ok                                 = false;
  fixture_.journal().async_commit               = true;
  fixture_.checkpoint_store().async_materialize = true;
  // SealAndStartSave wires OnCheckpointFinished; count via task ends + lifecycle.
  const auto close_outcome                      = fixture_.nav().RequestClose(true);
  ASSERT_TRUE(close_outcome.waiting_for_checkpoint);
  ASSERT_TRUE(close_outcome.ticket.valid());
  const int ends_before_close = fixture_.tasks().end_count;
  fixture_.CompleteCheckpoint();
  EXPECT_FALSE(fixture_.nav().has_pending_action());
  EXPECT_EQ(fixture_.tasks().end_count, ends_before_close + 1);
  close_terminals = fixture_.tasks().end_count - ends_before_close;
  close_ok        = fixture_.tasks().ended_success.back();
  EXPECT_EQ(close_terminals, 1);
  EXPECT_TRUE(close_ok);

  // Shutdown/cancel path: one terminal cancellation; late completions do not double-end.
  fixture_.TearDown();
  fixture_.SetUp();
  fixture_.OpenA();
  const auto switch_result = fixture_.RequestSwitchToB();
  ASSERT_TRUE(switch_result.waiting_for_checkpoint);
  int       cancel_terminals   = 0;
  bool      cancel_completed   = true;
  // Replace is not possible; CancelAndWait invokes the navigation completion once.
  const int ends_before_cancel = fixture_.tasks().end_count;
  fixture_.save_service().CancelAndWait();
  EXPECT_EQ(fixture_.tasks().end_count, ends_before_cancel + 1);
  cancel_terminals = fixture_.tasks().end_count - ends_before_cancel;
  cancel_completed = fixture_.tasks().ended_success.back();
  EXPECT_EQ(cancel_terminals, 1);
  EXPECT_FALSE(cancel_completed);
  // Late journal/materialize completions must not finish the task again.
  fixture_.journal().CompleteCommit(true);
  fixture_.checkpoint_store().CompleteMaterialization(true);
  EXPECT_EQ(fixture_.tasks().end_count, ends_before_cancel + 1);
  EXPECT_EQ(fixture_.lifecycle().identity().element_id,
            test::EditorSessionNavigationFixture::kElementA);
  EXPECT_EQ(std::count(fixture_.events().begin(), fixture_.events().end(), "acquire_b"), 0);
}

TEST_F(EditorSessionNavigationControllerTest, CaptureFailureKeepsAAndNeverStartsSave) {
  fixture_.OpenA();
  fixture_.history().fail_capture = true;

  const auto result               = fixture_.RequestSwitchToB();
  EXPECT_TRUE(result.failed);
  EXPECT_FALSE(result.waiting_for_checkpoint);
  EXPECT_FALSE(fixture_.nav().has_pending_action());
  EXPECT_EQ(fixture_.lifecycle().identity().element_id,
            test::EditorSessionNavigationFixture::kElementA);
  EXPECT_EQ(fixture_.checkpoint_store().materialize_count, 0);
  EXPECT_EQ(std::count(fixture_.events().begin(), fixture_.events().end(), "acquire_b"), 0);
}

}  // namespace
}  // namespace alcedo
