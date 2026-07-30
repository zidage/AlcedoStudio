//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include "app/editor_session_command_queue.hpp"
#include "support/editor_save_checkpoint_fixture.hpp"

namespace alcedo {
namespace {

class EditorSaveCheckpointServiceTest : public ::testing::Test {
 protected:
  void SetUp() override { fixture_.SetUp(); }
  void TearDown() override { fixture_.TearDown(); }

  test::EditorSaveCheckpointFixture fixture_;
};

TEST_F(EditorSaveCheckpointServiceTest, TaskBeginFailureReturnsInvalidTicket) {
  fixture_.fail_task_start = true;

  bool                 completion_called = false;
  SaveCheckpointResult completion_result;

  const auto ticket = fixture_.StartCheckpoint(42, 7, [&](const SaveCheckpointResult& result) {
    completion_called = true;
    completion_result = result;
  });

  EXPECT_FALSE(ticket.valid());
  EXPECT_TRUE(completion_called);
  EXPECT_FALSE(completion_result.checkpoint_completed);
  EXPECT_EQ(completion_result.error, "Failed to start editor save task");
  EXPECT_EQ(fixture_.tasks().end_count, 0);
}

TEST_F(EditorSaveCheckpointServiceTest, AsynchronousSuccessEndsTaskAndCompletesCheckpoint) {
  bool                 completion_called = false;
  SaveCheckpointResult completion_result;

  const auto ticket = fixture_.StartCheckpoint(42, 7, [&](const SaveCheckpointResult& result) {
    completion_called = true;
    completion_result = result;
  });

  EXPECT_TRUE(ticket.valid());
  EXPECT_NE(ticket.task_id, 0u);
  EXPECT_EQ(ticket.image_load_request_id.value, 7u);
  EXPECT_FALSE(completion_called);
  EXPECT_TRUE(fixture_.service().active());

  fixture_.CompleteJournalTruncate(true);
  EXPECT_FALSE(completion_called);
  EXPECT_TRUE(fixture_.service().active());

  fixture_.CompleteDatabaseWrite(true);
  EXPECT_TRUE(completion_called);
  EXPECT_TRUE(completion_result.checkpoint_completed);
  EXPECT_EQ(completion_result.task_id, ticket.task_id);
  EXPECT_EQ(fixture_.tasks().end_count, 1);
  EXPECT_TRUE(fixture_.tasks().ended_success.front());
  EXPECT_FALSE(fixture_.service().active());
  EXPECT_EQ(fixture_.thumbnails().refresh_count, 1);
  ASSERT_EQ(fixture_.thumbnails().refreshed_ids.size(), 1u);
  EXPECT_EQ(fixture_.thumbnails().refreshed_ids.front(), 42u);
}

TEST_F(EditorSaveCheckpointServiceTest, AsynchronousMaterializationFailureReportsFailure) {
  bool                 completion_called = false;
  SaveCheckpointResult completion_result;

  fixture_.StartCheckpoint(42, 7, [&](const SaveCheckpointResult& result) {
    completion_called = true;
    completion_result = result;
  });

  fixture_.CompleteJournalTruncate(true);
  fixture_.CompleteDatabaseWrite(false, "A materialization failed");

  EXPECT_TRUE(completion_called);
  EXPECT_FALSE(completion_result.checkpoint_completed);
  EXPECT_EQ(completion_result.error, "A materialization failed");
  EXPECT_EQ(fixture_.tasks().end_count, 1);
  EXPECT_FALSE(fixture_.tasks().ended_success.front());
  EXPECT_FALSE(fixture_.service().active());
  EXPECT_EQ(fixture_.thumbnails().refresh_count, 0);
}

TEST_F(EditorSaveCheckpointServiceTest, StaleOnCheckpointFinishedIsIgnored) {
  bool       completion_called = false;
  const auto ticket            = fixture_.StartCheckpoint(
      42, 7, [&](const SaveCheckpointResult&) { completion_called = true; });

  SaveCheckpointResult stale;
  stale.request_id           = 999;
  stale.image_load_request_id   = ImageLoadRequestId{7};
  stale.checkpoint_completed = true;
  fixture_.service().OnCheckpointFinished(stale);
  EXPECT_FALSE(completion_called);
  EXPECT_TRUE(fixture_.service().active());

  fixture_.CompleteJournalTruncate(true);
  fixture_.CompleteDatabaseWrite(true);
  EXPECT_TRUE(completion_called);
  EXPECT_FALSE(fixture_.service().active());
  EXPECT_NE(ticket.request_id, 0u);
}

TEST_F(EditorSaveCheckpointServiceTest, DuplicateOnCheckpointFinishedDoesNotDoubleEndTask) {
  const auto ticket = fixture_.StartCheckpoint(42, 7, [](const SaveCheckpointResult&) {});

  fixture_.CompleteJournalTruncate(true);
  fixture_.CompleteDatabaseWrite(true);
  ASSERT_EQ(fixture_.tasks().end_count, 1);

  SaveCheckpointResult dup;
  dup.request_id           = ticket.request_id;
  dup.image_load_request_id   = ticket.image_load_request_id;
  dup.checkpoint_completed = true;
  fixture_.service().OnCheckpointFinished(dup);
  EXPECT_EQ(fixture_.tasks().end_count, 1);
}

TEST_F(EditorSaveCheckpointServiceTest, CancelAndWaitStopsCallbacksAndJoins) {
  int                  completion_count = 0;
  SaveCheckpointResult completion_result;
  fixture_.StartCheckpoint(42, 7, [&](const SaveCheckpointResult& result) {
    ++completion_count;
    completion_result = result;
  });

  fixture_.CancelAndWait();
  // Exactly one terminal cancellation; later journal/materialize cannot complete again.
  EXPECT_EQ(completion_count, 1);
  EXPECT_FALSE(completion_result.checkpoint_completed);
  EXPECT_NE(completion_result.error.find("cancelled"), std::string::npos);
  EXPECT_EQ(fixture_.tasks().end_count, 1);
  EXPECT_FALSE(fixture_.tasks().ended_success.front());

  fixture_.CompleteJournalTruncate(true);
  EXPECT_FALSE(static_cast<bool>(fixture_.checkpoint_store().pending_materialize));
  EXPECT_EQ(completion_count, 1);
  EXPECT_EQ(fixture_.tasks().end_count, 1);
}

TEST_F(EditorSaveCheckpointServiceTest, MaterializeStartFailureCompletesWithFailure) {
  bool                 completion_called = false;
  SaveCheckpointResult completion_result;
  fixture_.StartCheckpoint(42, 7, [&](const SaveCheckpointResult& result) {
    completion_called = true;
    completion_result = result;
  });

  fixture_.checkpoint_store().fail_materialize_start = true;
  fixture_.CompleteJournalTruncate(true);
  EXPECT_TRUE(completion_called);
  EXPECT_FALSE(completion_result.checkpoint_completed);
  EXPECT_EQ(completion_result.error, "Materialization could not start");
  EXPECT_EQ(fixture_.tasks().end_count, 1);
  EXPECT_FALSE(fixture_.tasks().ended_success.front());
}

TEST_F(EditorSaveCheckpointServiceTest, StaleSessionGenerationIsIgnoredByOnCheckpointFinished) {
  bool       completion_called = false;
  const auto ticket            = fixture_.StartCheckpoint(
      42, 7, [&](const SaveCheckpointResult&) { completion_called = true; });

  SaveCheckpointResult mismatched;
  mismatched.request_id           = ticket.request_id;
  mismatched.image_load_request_id   = ImageLoadRequestId{99};
  mismatched.checkpoint_completed = true;
  fixture_.service().OnCheckpointFinished(mismatched);
  EXPECT_FALSE(completion_called);
  EXPECT_TRUE(fixture_.service().active());

  fixture_.CompleteJournalTruncate(true);
  fixture_.CompleteDatabaseWrite(true);
  EXPECT_TRUE(completion_called);
  EXPECT_FALSE(fixture_.service().active());
}

/// Navigation callbacks can recover the next image through the same coordinator.
TEST_F(EditorSaveCheckpointServiceTest, GlobalSaveLockReleasesBeforeTerminalCallback) {
  bool       completion_called           = false;
  bool       lock_released_in_completion = false;
  const auto ticket = fixture_.StartCheckpoint(42, 7, [&](const SaveCheckpointResult&) {
    completion_called           = true;
    lock_released_in_completion = !fixture_.coordinator().is_saving();
  });
  ASSERT_TRUE(ticket.valid());
  EXPECT_TRUE(fixture_.coordinator().is_saving());
  EXPECT_EQ(fixture_.coordinator().active_element_id(), 42u);

  fixture_.CompleteJournalTruncate(true);
  EXPECT_FALSE(completion_called);
  EXPECT_TRUE(fixture_.coordinator().is_saving());

  fixture_.CompleteDatabaseWrite(true);
  EXPECT_TRUE(completion_called);
  EXPECT_TRUE(lock_released_in_completion);
  EXPECT_FALSE(fixture_.coordinator().is_saving());
  EXPECT_EQ(fixture_.coordinator().active_element_id(), 0u);
}

/// A second Start cannot take the global lock while the first save still owns it.
TEST_F(EditorSaveCheckpointServiceTest, SecondStartFailsWhileGlobalSaveLockIsHeld) {
  bool first_done  = false;
  bool second_done = false;
  SaveCheckpointResult second_result;

  const auto first =
      fixture_.StartCheckpoint(42, 7, [&](const SaveCheckpointResult&) { first_done = true; });
  ASSERT_TRUE(first.valid());
  EXPECT_TRUE(fixture_.coordinator().is_saving());

  const auto second = fixture_.StartCheckpoint(43, 8, [&](const SaveCheckpointResult& result) {
    second_done   = true;
    second_result = result;
  });
  EXPECT_FALSE(second.valid());
  EXPECT_TRUE(second_done);
  EXPECT_FALSE(second_result.checkpoint_completed);
  EXPECT_NE(second_result.error.find("global save lock"), std::string::npos);

  fixture_.CompleteJournalTruncate(true);
  fixture_.CompleteDatabaseWrite(true);
  EXPECT_TRUE(first_done);
  EXPECT_FALSE(fixture_.coordinator().is_saving());
}

/// CancelAndWait releases the project-owned save lock and publishes one terminal.
TEST_F(EditorSaveCheckpointServiceTest, CancelAndWaitReleasesGlobalSaveLock) {
  int completion_count = 0;
  ASSERT_TRUE(fixture_
                  .StartCheckpoint(42, 7, [&](const SaveCheckpointResult&) {
                    ++completion_count;
                  })
                  .valid());
  EXPECT_TRUE(fixture_.coordinator().is_saving());

  fixture_.CancelAndWait();
  EXPECT_EQ(completion_count, 1);
  EXPECT_FALSE(fixture_.coordinator().is_saving());
}

/// Phase 4A: missing capture must not report a successful no-op after journal durability.
TEST_F(EditorSaveCheckpointServiceTest, ConfiguredProjectWithoutHistoryStoreOrStorageFails) {
  fixture_.fail_capture = true;
  bool                 done = false;
  SaveCheckpointResult result;
  ASSERT_TRUE(fixture_
                  .StartCheckpoint(42, 7, [&](const SaveCheckpointResult& r) {
                    done   = true;
                    result = r;
                  })
                  .valid());
  fixture_.CompleteJournalTruncate(true);
  EXPECT_TRUE(done);
  EXPECT_FALSE(result.checkpoint_completed);
  EXPECT_NE(result.error.find("capture"), std::string::npos);
  EXPECT_EQ(fixture_.checkpoint_store().materialize_count, 0);
  EXPECT_EQ(fixture_.tasks().end_count, 1);
  EXPECT_FALSE(fixture_.tasks().ended_success.front());
}

TEST_F(EditorSaveCheckpointServiceTest, MissingCheckpointStoreFailsAfterDurableJournal) {
  auto journal          = std::make_shared<test::FakeEditorJournalPort>();
  auto tasks            = std::make_shared<test::FakeEditorTaskPort>();
  auto coordinator      = std::make_shared<EditorSaveCheckpointCoordinator>();
  auto thumbnails       = std::make_shared<test::FakeEditorThumbnailPort>();
  auto executor         = std::make_shared<EditorSessionManualCommandExecutor>();
  journal->async_commit = true;

  EditorSaveCheckpointService::Dependencies deps;
  deps.journal          = journal;
  deps.tasks            = tasks;
  deps.save_coordinator = coordinator;
  deps.checkpoint_store = nullptr;
  deps.thumbnails       = thumbnails;
  deps.command_executor = executor;
  EditorSaveCheckpointService service(std::move(deps));

  bool                 done = false;
  SaveCheckpointResult result;
  auto                 save_lock = service.TryAcquireSaveLock(42);
  ASSERT_TRUE(save_lock.owns_lock());
  SaveCheckpointRequest request;
  request.element_id         = 42;
  request.image_load_request_id = ImageLoadRequestId{7};
  request.capture            = test::MakeOpaqueSaveCapture();
  request.save_lock          = std::move(save_lock);
  const auto ticket =
      service.Start(std::move(request), [&](const SaveCheckpointResult& r) {
        done   = true;
        result = r;
      });
  ASSERT_TRUE(ticket.valid());
  journal->CompleteCommit(true);
  executor->DrainAll();
  EXPECT_TRUE(done);
  EXPECT_FALSE(result.checkpoint_completed);
  EXPECT_NE(result.error.find("store"), std::string::npos);
  EXPECT_EQ(tasks->end_count, 1);
  EXPECT_FALSE(tasks->ended_success.front());
  EXPECT_EQ(thumbnails->refresh_count, 0);
  service.CancelAndWait();
  executor->DrainAll();
  coordinator->Shutdown();
}

TEST_F(EditorSaveCheckpointServiceTest, CapturePointerReachesCheckpointStoreWithoutSideMap) {
  bool                 done = false;
  SaveCheckpointResult result;
  const auto           capture = test::MakeOpaqueSaveCapture();
  auto                 save_lock = fixture_.service().TryAcquireSaveLock(42);
  ASSERT_TRUE(save_lock.owns_lock());
  SaveCheckpointRequest request;
  request.element_id         = 42;
  request.image_load_request_id = ImageLoadRequestId{7};
  request.capture            = capture;
  request.save_lock          = std::move(save_lock);
  const auto ticket =
      fixture_.service().Start(std::move(request), [&](const SaveCheckpointResult& r) {
        done   = true;
        result = r;
      });
  ASSERT_TRUE(ticket.valid());
  fixture_.DrainCompletions();
  fixture_.CompleteJournalTruncate(true);
  EXPECT_EQ(fixture_.checkpoint_store().last_capture.get(), capture.get());
  fixture_.CompleteDatabaseWrite(true);
  EXPECT_TRUE(done);
  EXPECT_TRUE(result.checkpoint_completed);
  EXPECT_EQ(fixture_.checkpoint_store().materialize_count, 1);
}

}  // namespace
}  // namespace alcedo
