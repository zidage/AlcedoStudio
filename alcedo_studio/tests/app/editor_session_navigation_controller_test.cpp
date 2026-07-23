//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include "support/editor_session_navigation_fixture.hpp"

namespace alcedo {
namespace {

class EditorSessionNavigationControllerTest : public ::testing::Test {
 protected:
  void SetUp() override { fixture_.SetUp(); }
  void TearDown() override { fixture_.TearDown(); }

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

TEST_F(EditorSessionNavigationControllerTest, SwitchToBWaitsForASaveCheckpoint) {
  fixture_.OpenA();

  const auto result = fixture_.RequestSwitchToB();
  EXPECT_TRUE(result.waiting_for_checkpoint);
  EXPECT_FALSE(result.completed_synchronously);
  EXPECT_TRUE(fixture_.nav().has_pending_action());
  EXPECT_TRUE(result.ticket.valid());
  ASSERT_FALSE(fixture_.events().empty());
  EXPECT_EQ(fixture_.events().front(), "checkpoint_a");

  fixture_.CompleteCheckpoint();
  EXPECT_FALSE(fixture_.nav().has_pending_action());
  EXPECT_TRUE(fixture_.lifecycle().has_image());
  EXPECT_EQ(fixture_.lifecycle().identity().element_id,
            test::EditorSessionNavigationFixture::kElementB);
  ASSERT_GE(fixture_.events().size(), 3u);
  EXPECT_EQ(fixture_.events()[0], "checkpoint_a");
  EXPECT_EQ(fixture_.events()[1], "release_a");
  EXPECT_EQ(fixture_.events()[2], "acquire_b");
}

TEST_F(EditorSessionNavigationControllerTest, SaveFailureKeepsAAndDiscardsPendingAction) {
  fixture_.OpenA();

  const auto result = fixture_.RequestSwitchToB();
  EXPECT_TRUE(result.waiting_for_checkpoint);

  fixture_.FailCheckpoint("A materialization failed");
  EXPECT_FALSE(fixture_.nav().has_pending_action());
  EXPECT_EQ(fixture_.lifecycle().state(), EditorSessionState::Failed);
  EXPECT_EQ(fixture_.lifecycle().identity().element_id,
            test::EditorSessionNavigationFixture::kElementA);
}

TEST_F(EditorSessionNavigationControllerTest, SecondActionDoesNotReplaceOriginalTarget) {
  fixture_.OpenA();

  const auto first = fixture_.RequestSwitchToB();
  EXPECT_TRUE(first.waiting_for_checkpoint);
  EXPECT_TRUE(fixture_.nav().has_pending_action());

  const auto second = fixture_.nav().RequestOpenOrSwitch(5, 6, true);
  EXPECT_TRUE(second.rejected);
  EXPECT_EQ(second.message, "Editor save checkpoint is in progress");
  EXPECT_TRUE(fixture_.nav().has_pending_action());
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

TEST_F(EditorSessionNavigationControllerTest, SyncSaveSwitchCompletesImmediately) {
  fixture_.OpenA();
  const auto gen_a = fixture_.lifecycle().identity().session_generation;

  // Default journal path is synchronous when async_commit remains false.
  fixture_.journal().async_commit               = false;
  fixture_.checkpoint_store().async_materialize = false;
  const auto result = fixture_.nav().RequestOpenOrSwitch(
      test::EditorSessionNavigationFixture::kElementB, test::EditorSessionNavigationFixture::kImageB,
      true);
  EXPECT_TRUE(result.completed_synchronously);
  EXPECT_FALSE(result.waiting_for_checkpoint);
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

TEST_F(EditorSessionNavigationControllerTest, SyncSaveFailureStaysOnA) {
  fixture_.journal().fail_barrier = true;
  fixture_.OpenA();
  const auto gen_a = fixture_.lifecycle().identity().session_generation;

  fixture_.journal().async_commit               = false;
  fixture_.checkpoint_store().async_materialize = false;
  const auto result = fixture_.nav().RequestOpenOrSwitch(
      test::EditorSessionNavigationFixture::kElementB, test::EditorSessionNavigationFixture::kImageB,
      true);
  EXPECT_TRUE(result.failed);
  EXPECT_FALSE(result.completed_synchronously);
  EXPECT_FALSE(result.waiting_for_checkpoint);
  EXPECT_FALSE(fixture_.nav().has_pending_action());
  EXPECT_EQ(fixture_.lifecycle().identity().session_generation, gen_a);
  EXPECT_EQ(fixture_.lifecycle().state(), EditorSessionState::Failed);
}

TEST_F(EditorSessionNavigationControllerTest, SyncCloseCompletesImmediately) {
  fixture_.OpenA();
  fixture_.journal().async_commit               = false;
  fixture_.checkpoint_store().async_materialize = false;
  const auto result                             = fixture_.nav().RequestClose(true);
  EXPECT_TRUE(result.completed_synchronously);
  EXPECT_FALSE(result.waiting_for_checkpoint);
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

}  // namespace
}  // namespace alcedo
