//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_save_checkpoint_service.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "app/editor_session_ports.hpp"

namespace alcedo {
namespace {

auto MakeOpaqueCapture() -> std::shared_ptr<const EditorMiniGitSaveCapture> {
  static const int token = 0;
  return {reinterpret_cast<const EditorMiniGitSaveCapture*>(&token),
          [](const EditorMiniGitSaveCapture*) {}};
}

// ── Fakes ────────────────────────────────────────────────────────────────────

class FakeTaskPort final : public IEditorTaskPort {
 public:
  bool                       fail_begin  = false;
  int                        begin_count = 0;
  int                        end_count   = 0;
  std::vector<std::uint64_t> begun_ids;
  std::vector<std::uint64_t> ended_ids;
  std::vector<bool>          ended_success;
  std::vector<std::string>   ended_messages;
  std::uint64_t              next_id = 1;

  auto                       BeginTask(const std::string& /*name*/, sl_element_id_t /*element_id*/)
      -> std::uint64_t override {
    ++begin_count;
    if (fail_begin) {
      return 0;
    }
    const auto id = next_id++;
    begun_ids.push_back(id);
    return id;
  }
  void EndTask(std::uint64_t task_id, bool success, const std::string& message) override {
    ++end_count;
    ended_ids.push_back(task_id);
    ended_success.push_back(success);
    ended_messages.push_back(message);
  }
};

class FakeJournalPort final : public IEditorJournalPort {
 public:
  bool                        fail_barrier  = false;
  bool                        async_commit  = false;
  int                         barrier_count = 0;
  EditorJournalCommitCallback pending_commit;

  auto AppendBarrier(sl_element_id_t, std::uint64_t, std::string* error) -> bool override {
    ++barrier_count;
    if (fail_barrier) {
      if (error) {
        *error = "journal barrier failed";
      }
      return false;
    }
    return true;
  }

  auto CommitJournalAsync(sl_element_id_t element_id, std::uint64_t session_generation,
                          EditorJournalCommitCallback callback) -> bool override {
    if (!async_commit) {
      return IEditorJournalPort::CommitJournalAsync(element_id, session_generation,
                                                    std::move(callback));
    }
    pending_commit = std::move(callback);
    return true;
  }

  void CompleteCommit(bool durable, std::string error = {}) {
    ASSERT_TRUE(static_cast<bool>(pending_commit));
    auto callback = std::move(pending_commit);
    callback(EditorJournalCommitOutcome{true, durable, !durable, durable ? 2u : 0u,
                                        durable ? 1u : 0u, std::move(error)});
  }

  auto DiscardUnflushed(sl_element_id_t, std::string*) -> bool override { return true; }
};

class FakeCheckpointStore final : public IEditorCheckpointStore {
 public:
  bool                      async_materialize      = false;
  bool                      fail_materialize_start = false;
  EditorMaterializeCallback pending_materialize;

  auto MaterializeAsync(std::shared_ptr<const EditorMiniGitSaveCapture> capture,
                        EditorMaterializeCallback                       callback) -> bool override {
    if (fail_materialize_start) {
      return false;
    }
    if (!async_materialize) {
      return IEditorCheckpointStore::MaterializeAsync(std::move(capture), std::move(callback));
    }
    pending_materialize = std::move(callback);
    return true;
  }

  void CompleteMaterialization(bool materialized, std::string error = {}) {
    ASSERT_TRUE(static_cast<bool>(pending_materialize));
    auto callback = std::move(pending_materialize);
    callback(
        EditorMaterializeOutcome{true, materialized, materialized ? 1u : 0u, std::move(error)});
  }
};

// ── Fixture ──────────────────────────────────────────────────────────────────

class EditorSaveCheckpointServiceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    tasks_            = std::make_shared<FakeTaskPort>();
    journal_          = std::make_shared<FakeJournalPort>();
    checkpoint_store_ = std::make_shared<FakeCheckpointStore>();

    EditorSaveCheckpointService::Dependencies deps;
    deps.journal          = journal_;
    deps.checkpoint_store = checkpoint_store_;
    deps.tasks            = tasks_;
    service_              = std::make_unique<EditorSaveCheckpointService>(std::move(deps));
  }

  auto MakeRequest() -> SaveCheckpointRequest {
    SaveCheckpointRequest req;
    req.element_id         = 42;
    req.session_generation = 7;
    req.capture            = MakeOpaqueCapture();
    return req;
  }

  std::shared_ptr<FakeTaskPort>                tasks_;
  std::shared_ptr<FakeJournalPort>             journal_;
  std::shared_ptr<FakeCheckpointStore>         checkpoint_store_;
  std::unique_ptr<EditorSaveCheckpointService> service_;
};

// ── Tests ────────────────────────────────────────────────────────────────────

TEST_F(EditorSaveCheckpointServiceTest, TaskBeginFailureReturnsInvalidTicket) {
  tasks_->fail_begin                     = true;

  bool                 completion_called = false;
  SaveCheckpointResult completion_result;

  const auto ticket = service_->Start(MakeRequest(), [&](const SaveCheckpointResult& result) {
    completion_called = true;
    completion_result = result;
  });

  EXPECT_FALSE(ticket.valid());
  EXPECT_TRUE(completion_called);
  EXPECT_FALSE(completion_result.checkpoint_completed);
  EXPECT_EQ(completion_result.error, "Failed to start editor save task");
  EXPECT_EQ(tasks_->end_count, 0);
}

TEST_F(EditorSaveCheckpointServiceTest, AsynchronousSuccessEndsTaskAndCompletesCheckpoint) {
  journal_->async_commit                 = true;
  checkpoint_store_->async_materialize   = true;

  bool                 completion_called = false;
  SaveCheckpointResult completion_result;

  const auto ticket = service_->Start(MakeRequest(), [&](const SaveCheckpointResult& result) {
    completion_called = true;
    completion_result = result;
  });

  EXPECT_TRUE(ticket.valid());
  EXPECT_NE(ticket.task_id, 0u);
  EXPECT_EQ(ticket.session_generation, 7u);
  EXPECT_FALSE(completion_called);
  EXPECT_TRUE(service_->active());

  journal_->CompleteCommit(true);
  EXPECT_FALSE(completion_called);
  EXPECT_TRUE(service_->active());

  checkpoint_store_->CompleteMaterialization(true);
  EXPECT_TRUE(completion_called);
  EXPECT_TRUE(completion_result.checkpoint_completed);
  EXPECT_EQ(completion_result.task_id, ticket.task_id);
  EXPECT_EQ(tasks_->end_count, 1);
  EXPECT_TRUE(tasks_->ended_success.front());
  EXPECT_FALSE(service_->active());
}

TEST_F(EditorSaveCheckpointServiceTest, AsynchronousMaterializationFailureReportsFailure) {
  journal_->async_commit                 = true;
  checkpoint_store_->async_materialize   = true;

  bool                 completion_called = false;
  SaveCheckpointResult completion_result;

  service_->Start(MakeRequest(), [&](const SaveCheckpointResult& result) {
    completion_called = true;
    completion_result = result;
  });

  journal_->CompleteCommit(true);
  checkpoint_store_->CompleteMaterialization(false, "A materialization failed");

  EXPECT_TRUE(completion_called);
  EXPECT_FALSE(completion_result.checkpoint_completed);
  EXPECT_EQ(completion_result.error, "A materialization failed");
  EXPECT_EQ(tasks_->end_count, 1);
  EXPECT_FALSE(tasks_->ended_success.front());
  EXPECT_FALSE(service_->active());
}

TEST_F(EditorSaveCheckpointServiceTest, StaleOnCheckpointFinishedIsIgnored) {
  journal_->async_commit               = true;
  checkpoint_store_->async_materialize = true;

  bool       completion_called         = false;
  const auto ticket                    = service_->Start(
      MakeRequest(), [&](const SaveCheckpointResult&) { completion_called = true; });

  // A result for a request_id that has no pending save is silently ignored.
  SaveCheckpointResult stale;
  stale.request_id           = 999;  // different from ticket.request_id
  stale.session_generation   = 7;
  stale.checkpoint_completed = true;
  service_->OnCheckpointFinished(stale);
  EXPECT_FALSE(completion_called);
  EXPECT_TRUE(service_->active());

  // Complete the real save.
  journal_->CompleteCommit(true);
  checkpoint_store_->CompleteMaterialization(true);
  EXPECT_TRUE(completion_called);
  EXPECT_FALSE(service_->active());
}

TEST_F(EditorSaveCheckpointServiceTest, DuplicateOnCheckpointFinishedDoesNotDoubleEndTask) {
  journal_->async_commit               = true;
  checkpoint_store_->async_materialize = true;

  const auto ticket = service_->Start(MakeRequest(), [](const SaveCheckpointResult&) {});

  journal_->CompleteCommit(true);
  checkpoint_store_->CompleteMaterialization(true);
  ASSERT_EQ(tasks_->end_count, 1);

  // Duplicate completion for the same request_id: the pending save was already
  // erased, so this is a no-op.
  SaveCheckpointResult dup;
  dup.request_id           = ticket.request_id;
  dup.session_generation   = ticket.session_generation;
  dup.checkpoint_completed = true;
  service_->OnCheckpointFinished(dup);
  EXPECT_EQ(tasks_->end_count, 1);
}

TEST_F(EditorSaveCheckpointServiceTest, CancelAndWaitStopsCallbacksAndJoins) {
  journal_->async_commit               = true;
  checkpoint_store_->async_materialize = true;

  bool completion_called               = false;
  service_->Start(MakeRequest(), [&](const SaveCheckpointResult&) { completion_called = true; });

  service_->CancelAndWait();
  // After CancelAndWait, completing the journal should not invoke the
  // completion callback because the gate rejects new entries.
  journal_->CompleteCommit(true);
  EXPECT_FALSE(static_cast<bool>(checkpoint_store_->pending_materialize));
  EXPECT_FALSE(completion_called);
}

TEST_F(EditorSaveCheckpointServiceTest, MaterializeStartFailureCompletesWithFailure) {
  journal_->async_commit                    = true;
  checkpoint_store_->fail_materialize_start = true;

  bool                 completion_called    = false;
  SaveCheckpointResult completion_result;
  service_->Start(MakeRequest(), [&](const SaveCheckpointResult& result) {
    completion_called = true;
    completion_result = result;
  });

  journal_->CompleteCommit(true);
  EXPECT_TRUE(completion_called);
  EXPECT_FALSE(completion_result.checkpoint_completed);
  EXPECT_EQ(completion_result.error, "Materialization could not start");
  EXPECT_EQ(tasks_->end_count, 1);
  EXPECT_FALSE(tasks_->ended_success.front());
}

TEST_F(EditorSaveCheckpointServiceTest, StaleSessionGenerationIsIgnoredByOnCheckpointFinished) {
  journal_->async_commit               = true;
  checkpoint_store_->async_materialize = true;

  bool       completion_called         = false;
  const auto ticket                    = service_->Start(
      MakeRequest(), [&](const SaveCheckpointResult&) { completion_called = true; });

  // A result with a different session_generation must not consume the pending
  // save even when its request_id matches.
  SaveCheckpointResult mismatched;
  mismatched.request_id           = ticket.request_id;
  mismatched.session_generation   = 99;  // wrong session
  mismatched.checkpoint_completed = true;
  service_->OnCheckpointFinished(mismatched);
  EXPECT_FALSE(completion_called);
  EXPECT_TRUE(service_->active());

  journal_->CompleteCommit(true);
  checkpoint_store_->CompleteMaterialization(true);
  EXPECT_TRUE(completion_called);
  EXPECT_FALSE(service_->active());
}

}  // namespace
}  // namespace alcedo
