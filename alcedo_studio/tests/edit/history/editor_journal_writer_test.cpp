//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "edit/history/edit_transaction.hpp"
#include "edit/history/editor_journal_writer.hpp"
#include "edit/history/editor_transaction_journal.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "type/hash_type.hpp"
#include "type/type.hpp"

namespace alcedo {
namespace {

auto MakeExposureTransaction(float exposure) -> EditTransaction {
  EditTransaction transaction{TransactionType::_EDIT,
                              OperatorType::EXPOSURE,
                              PipelineStageName::Basic_Adjustment,
                              nlohmann::json{{"exposure", 0.0f}},
                              nlohmann::json{{"exposure", exposure}},
                              true,
                              true};
  transaction.SetTransactionID(1);
  transaction.GenerateTransactionHash();
  return transaction;
}

struct MemoryJournalFile final : IEditorJournalFile {
  std::vector<std::uint8_t> bytes;
  std::size_t               max_write = 0;
  int                       append_calls = 0;
  int                       flush_calls  = 0;
  bool                      fail_flush   = false;

  auto Append(const std::uint8_t* data, std::size_t size, std::size_t* written,
              std::string* error) -> bool override {
    ++append_calls;
    if (written == nullptr) {
      if (error) {
        *error = "missing write count";
      }
      return false;
    }
    const auto count = max_write == 0 ? size : std::min(size, max_write);
    bytes.insert(bytes.end(), data, data + count);
    *written = count;
    return true;
  }

  auto Flush(std::string* error) -> bool override {
    ++flush_calls;
    if (fail_flush) {
      if (error) {
        *error = "injected flush failure";
      }
      return false;
    }
    return true;
  }
};

class EditorJournalWriterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RegisterAllOperators();
    identity_.element_id         = 42;
    identity_.version_id         = Hash128(11, 22);
    identity_.session_generation = 7;
    identity_.journal_generation = 1;
    file_ = std::make_shared<MemoryJournalFile>();
    writer_ = std::make_unique<EditorJournalWriter>(&journal_, file_);
  }

  EditorJournalIdentity                    identity_{};
  EditorTransactionJournal                 journal_;
  std::shared_ptr<MemoryJournalFile>       file_;
  std::unique_ptr<EditorJournalWriter>     writer_;
};

}  // namespace

TEST_F(EditorJournalWriterTest, RecordsAfterLastJournalBatchCommitAreNotReplayed) {
  const auto first = MakeExposureTransaction(1.0f);
  ASSERT_NE(writer_->AppendEdit(identity_, first), 0u);
  ASSERT_TRUE(writer_->CommitQueued().durable);

  EditorTransactionJournal persisted;
  std::string               load_error;
  ASSERT_TRUE(persisted.LoadBytes(file_->bytes, &load_error)) << load_error;
  JournalTimelineSimulator persisted_simulator(identity_);
  ASSERT_EQ(persisted_simulator.ReplayCommittedRecordChain(persisted).status,
            EditorJournalApplyStatus::Applied);
  ASSERT_EQ(persisted_simulator.transactions().size(), 1u);
  EXPECT_DOUBLE_EQ(
      persisted_simulator.transactions().front().GetAfterParams()["exposure"].get<double>(), 1.0);

  auto second = MakeExposureTransaction(2.0f);
  second.SetTransactionID(2);
  second.GenerateTransactionHash();
  ASSERT_NE(writer_->AppendEdit(identity_, second), 0u);
  // Leave the second operation complete in memory but without its commit record.

  JournalTimelineSimulator simulator(identity_);
  const auto replay = simulator.ReplayCommittedRecordChain(journal_);
  ASSERT_EQ(replay.status, EditorJournalApplyStatus::Applied);
  ASSERT_EQ(simulator.transactions().size(), 1u);
  EXPECT_DOUBLE_EQ(simulator.transactions().front().GetAfterParams()["exposure"].get<double>(),
                   1.0);
}

TEST_F(EditorJournalWriterTest, JournalFlushAdvancesDurableSequenceOnlyAfterSuccessfulFileFlush) {
  const auto transaction = MakeExposureTransaction(1.0f);
  ASSERT_NE(writer_->AppendEdit(identity_, transaction), 0u);

  file_->fail_flush = true;
  const auto failed = writer_->CommitQueued();
  EXPECT_TRUE(failed.accepted);
  EXPECT_FALSE(failed.durable);
  EXPECT_TRUE(failed.pending);
  EXPECT_EQ(writer_->state().durable_batch_commit_sequence, 0u);
  EXPECT_EQ(writer_->state().durable_operation_sequence, 0u);
  EXPECT_EQ(writer_->state().written_record_sequence, 2u);

  file_->fail_flush = false;
  const auto retried = writer_->RetryPending();
  EXPECT_TRUE(retried.durable);
  EXPECT_EQ(writer_->state().durable_batch_commit_sequence, 2u);
  EXPECT_EQ(writer_->state().durable_operation_sequence, 1u);
}

TEST_F(EditorJournalWriterTest, ShortWritesAreCompletedBeforeJournalFlush) {
  file_->max_write = 3;
  ASSERT_NE(writer_->AppendEdit(identity_, MakeExposureTransaction(1.0f)), 0u);

  const auto committed = writer_->CommitQueued();
  ASSERT_TRUE(committed.durable);
  EXPECT_GT(file_->append_calls, 2);
  ASSERT_EQ(writer_->journal().DecodeRecordChain().records.size(), 2u);
}

TEST_F(EditorJournalWriterTest, FailedJournalFlushKeepsEditPendingAndBlocksNewAppends) {
  ASSERT_NE(writer_->AppendEdit(identity_, MakeExposureTransaction(1.0f)), 0u);
  file_->fail_flush = true;
  ASSERT_FALSE(writer_->CommitQueued().durable);

  auto second = MakeExposureTransaction(2.0f);
  second.SetTransactionID(2);
  second.GenerateTransactionHash();
  EXPECT_EQ(writer_->AppendEdit(identity_, second), 0u);
  EXPECT_TRUE(writer_->has_pending_batch());
}

TEST_F(EditorJournalWriterTest, RetryingFailedJournalFlushDoesNotDuplicateFrames) {
  ASSERT_NE(writer_->AppendEdit(identity_, MakeExposureTransaction(1.0f)), 0u);
  file_->fail_flush = true;
  ASSERT_FALSE(writer_->CommitQueued().durable);
  const auto append_calls_before_retry = file_->append_calls;

  file_->fail_flush = false;
  ASSERT_TRUE(writer_->RetryPending().durable);
  EXPECT_EQ(file_->append_calls, append_calls_before_retry);

  const auto decoded = journal_.DecodeRecordChain();
  ASSERT_FALSE(decoded.stopped_on_corrupt_record);
  ASSERT_EQ(decoded.records.size(), 2u);
  EXPECT_EQ(decoded.records[0].record_type, EditorJournalRecordType::EditAppend);
  EXPECT_EQ(decoded.records[1].record_type, EditorJournalRecordType::JournalBatchCommit);
}

TEST_F(EditorJournalWriterTest, ReplayingCommittedJournalTwiceIsIdempotent) {
  ASSERT_NE(writer_->AppendEdit(identity_, MakeExposureTransaction(1.0f)), 0u);
  ASSERT_TRUE(writer_->CommitQueued().durable);

  JournalTimelineSimulator simulator(identity_);
  ASSERT_EQ(simulator.ReplayCommittedRecordChain(journal_).status,
            EditorJournalApplyStatus::Applied);
  const auto first_hash = simulator.TimelineHash();
  const auto first_cursor = simulator.cursor();

  ASSERT_EQ(simulator.ReplayCommittedRecordChain(journal_).status,
            EditorJournalApplyStatus::Applied);
  EXPECT_EQ(simulator.TimelineHash(), first_hash);
  EXPECT_EQ(simulator.cursor(), first_cursor);
  EXPECT_EQ(simulator.transactions().size(), 1u);
}

TEST_F(EditorJournalWriterTest, DiscardQueuedTailRestoresTheLastCommittedPrefix) {
  ASSERT_NE(writer_->AppendEdit(identity_, MakeExposureTransaction(1.0f)), 0u);
  ASSERT_TRUE(writer_->CommitQueued().durable);
  const auto committed_size = journal_.size();

  auto second = MakeExposureTransaction(2.0f);
  second.SetTransactionID(2);
  second.GenerateTransactionHash();
  ASSERT_NE(writer_->AppendEdit(identity_, second), 0u);
  ASSERT_TRUE(writer_->DiscardQueued());

  EXPECT_EQ(journal_.size(), committed_size);
  EXPECT_EQ(journal_.next_sequence(), 3u);
  EXPECT_EQ(journal_.DecodeRecordChain().records.size(), 2u);
}

}  // namespace alcedo
