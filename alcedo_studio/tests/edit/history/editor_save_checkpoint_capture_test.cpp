//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "app/editor_mini_git_materializer.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "support/editor_mini_git_project_fixture.hpp"

namespace alcedo {
namespace {

class EditorSaveCheckpointCaptureTest : public ::testing::Test {
 protected:
  void SetUp() override { project_.SetUp(); }
  void TearDown() override { project_.TearDown(); }

  test::EditorMiniGitProjectFixture project_;
};

auto MakeHeadMoveRecord(head_commit_hash_t source, transaction_chain_hash_t source_chain,
                        head_commit_hash_t target, transaction_chain_hash_t target_chain)
    -> MiniGitJournalRecord {
  MiniGitJournalRecord record;
  record.kind                       = MiniGitJournalRecordKind::kHeadMove;
  record.expected_source_head       = source;
  record.expected_source_chain_hash = source_chain;
  record.target_head                = target;
  record.target_chain_hash          = target_chain;
  return record;
}

TEST_F(EditorSaveCheckpointCaptureTest, EmptyJournalCaptureHasNoSequenceRangeAndDoesNotNeedAnotherFlag) {
  auto capture = project_.CaptureWorkingState(test::EditorMiniGitProjectFixture::kElementA, 0.0f);

  EXPECT_TRUE(capture.journal_records.empty());
  EXPECT_FALSE(capture.first_journal_sequence.has_value());
  EXPECT_FALSE(capture.last_journal_sequence.has_value());
  EXPECT_FALSE(capture.has_journal_range());
  EXPECT_EQ(capture.element_id, test::EditorMiniGitProjectFixture::kElementA);
  EXPECT_EQ(capture.root_id, project_.root_id(test::EditorMiniGitProjectFixture::kElementA));
  EXPECT_EQ(capture.version_id,
            project_.graph(test::EditorMiniGitProjectFixture::kElementA)->GetActiveVersionId());

  std::string error;
  const auto  result = project_.MaterializeUnderSaveLock(capture, &error);
  ASSERT_TRUE(result.accepted) << error << " / " << result.error;
  ASSERT_TRUE(result.materialized);
  EXPECT_FALSE(result.head_moved);
}

TEST_F(EditorSaveCheckpointCaptureTest, CaptureContainsElementVersionRootHeadHashStateAndExactRecords) {
  ASSERT_TRUE(
      project_.AppendExposureEdit(test::EditorMiniGitProjectFixture::kElementA, 0.0f, 0.4f));
  ASSERT_TRUE(
      project_.AppendExposureEdit(test::EditorMiniGitProjectFixture::kElementA, 0.4f, 0.9f));

  auto        capture = project_.CaptureWorkingState(test::EditorMiniGitProjectFixture::kElementA, 0.9f);
  const auto& graph   = *project_.graph(test::EditorMiniGitProjectFixture::kElementA);

  EXPECT_EQ(capture.element_id, test::EditorMiniGitProjectFixture::kElementA);
  EXPECT_EQ(capture.version_id, graph.GetActiveVersionId());
  EXPECT_EQ(capture.root_id, graph.GetRootId());
  EXPECT_EQ(capture.working_head, graph.GetActiveVersionRef().head_commit_hash);
  EXPECT_EQ(capture.transaction_chain_hash,
            graph.ChainHashForHead(graph.GetActiveVersionRef().head_commit_hash));
  EXPECT_EQ(capture.journal_path, project_.journal_path(test::EditorMiniGitProjectFixture::kElementA));
  ASSERT_EQ(capture.journal_records.size(), 2u);
  ASSERT_TRUE(capture.has_journal_range());
  EXPECT_EQ(*capture.first_journal_sequence, 1u);
  EXPECT_EQ(*capture.last_journal_sequence, 2u);
  EXPECT_EQ(capture.journal_records.front().sequence, 1u);
  EXPECT_EQ(capture.journal_records.back().sequence, 2u);
  EXPECT_EQ(capture.journal_records.front().kind, MiniGitJournalRecordKind::kEditCommit);
  EXPECT_EQ(capture.journal_records.back().kind, MiniGitJournalRecordKind::kEditCommit);
  ASSERT_TRUE(capture.materialization.image_state.serialized_pipeline_state.has_value());
  EXPECT_FLOAT_EQ(test::EditorMiniGitProjectFixture::CheckpointDocumentExposure(
                      *capture.materialization.image_state.serialized_pipeline_state),
                  0.9f);
}

TEST_F(EditorSaveCheckpointCaptureTest, EditAppendedAfterCaptureIsNotDeletedWithCapturedRecords) {
  ASSERT_TRUE(
      project_.AppendExposureEdit(test::EditorMiniGitProjectFixture::kElementA, 0.0f, 0.5f));
  auto capture = project_.CaptureWorkingState(test::EditorMiniGitProjectFixture::kElementA, 0.5f);
  ASSERT_TRUE(capture.has_journal_range());
  ASSERT_EQ(capture.journal_records.size(), 1u);
  const auto captured_sequence = *capture.last_journal_sequence;

  // A later edit lands after the captured range. Truncating the capture must
  // leave that record durable for the next checkpoint / recovery path.
  ASSERT_TRUE(
      project_.AppendExposureEdit(test::EditorMiniGitProjectFixture::kElementA, 0.5f, 1.5f));
  const auto live_before = project_.journal(test::EditorMiniGitProjectFixture::kElementA).Snapshot();
  ASSERT_EQ(live_before.records.size(), 2u);
  EXPECT_EQ(live_before.records.back().sequence, captured_sequence + 1);

  std::string error;
  const auto  result = project_.MaterializeUnderSaveLock(capture, &error);
  ASSERT_TRUE(result.accepted) << error << " / " << result.error;
  ASSERT_TRUE(result.materialized);

  const auto remaining =
      project_.ReadJournalRecords(test::EditorMiniGitProjectFixture::kElementA, &error);
  ASSERT_EQ(remaining.size(), 1u) << error;
  EXPECT_EQ(remaining.front().sequence, captured_sequence + 1);
  EXPECT_EQ(remaining.front().kind, MiniGitJournalRecordKind::kEditCommit);
  ASSERT_TRUE(remaining.front().edit_commit.has_value());
  EXPECT_EQ(remaining.front().edit_commit->GetCommitHash(),
            project_.graph(test::EditorMiniGitProjectFixture::kElementA)
                ->GetActiveVersionRef()
                .head_commit_hash);

  EXPECT_EQ(project_.CountStoredCommits(test::EditorMiniGitProjectFixture::kElementA), 1u);
}

TEST_F(EditorSaveCheckpointCaptureTest, FailedCheckpointKeepsTheCapturedRecordsForRetry) {
  ASSERT_TRUE(
      project_.AppendExposureEdit(test::EditorMiniGitProjectFixture::kElementA, 0.0f, 0.75f));
  auto capture = project_.CaptureWorkingState(test::EditorMiniGitProjectFixture::kElementA, 0.75f);
  ASSERT_TRUE(capture.has_journal_range());
  const auto preserved_records = capture.journal_records;
  const auto preserved_first   = capture.first_journal_sequence;
  const auto preserved_last    = capture.last_journal_sequence;

  // Force fold/validation failure after capture so DuckDB does not commit and
  // the journal prefix must remain available for retry.
  capture.working_head = Hash128{0xdead, 0xbeef};

  std::string error;
  const auto  result = project_.MaterializeUnderSaveLock(capture, &error);
  EXPECT_FALSE(result.accepted);
  EXPECT_FALSE(result.materialized);

  const auto remaining =
      project_.ReadJournalRecords(test::EditorMiniGitProjectFixture::kElementA, &error);
  ASSERT_EQ(remaining.size(), preserved_records.size()) << error;
  EXPECT_EQ(remaining.front().sequence, *preserved_first);
  EXPECT_EQ(remaining.back().sequence, *preserved_last);
  EXPECT_EQ(project_.CountStoredCommits(test::EditorMiniGitProjectFixture::kElementA), 0u);

  // Retry with the original intact capture must succeed.
  auto retry = project_.CaptureWorkingState(test::EditorMiniGitProjectFixture::kElementA, 0.75f);
  EXPECT_EQ(retry.first_journal_sequence, preserved_first);
  EXPECT_EQ(retry.last_journal_sequence, preserved_last);
  const auto retry_result = project_.MaterializeUnderSaveLock(retry, &error);
  ASSERT_TRUE(retry_result.accepted) << error << " / " << retry_result.error;
  ASSERT_TRUE(retry_result.materialized);
  EXPECT_TRUE(
      project_.ReadJournalRecords(test::EditorMiniGitProjectFixture::kElementA, &error).empty())
      << error;
}

enum class CaptureMismatchKind {
  ElementId,
  VersionId,
  RootId,
  SequenceRange,
};

class EditorSaveCheckpointCaptureMismatchTest
    : public EditorSaveCheckpointCaptureTest,
      public ::testing::WithParamInterface<CaptureMismatchKind> {};

TEST_P(EditorSaveCheckpointCaptureMismatchTest,
       MismatchedElementVersionRootOrSequenceRangeStartsNoMaterialization) {
  ASSERT_TRUE(
      project_.AppendExposureEdit(test::EditorMiniGitProjectFixture::kElementA, 0.0f, 1.0f));
  auto capture = project_.CaptureWorkingState(test::EditorMiniGitProjectFixture::kElementA, 1.0f);
  ASSERT_TRUE(capture.has_journal_range());

  switch (GetParam()) {
    case CaptureMismatchKind::ElementId:
      capture.element_id = test::EditorMiniGitProjectFixture::kElementB;
      break;
    case CaptureMismatchKind::VersionId:
      capture.version_id = Hash128{0x1111, 0x2222};
      break;
    case CaptureMismatchKind::RootId:
      capture.root_id = Hash128{0x3333, 0x4444};
      break;
    case CaptureMismatchKind::SequenceRange:
      capture.last_journal_sequence = *capture.last_journal_sequence + 1;
      break;
  }

  std::string error;
  const auto  result = project_.MaterializeUnderSaveLock(capture, &error);
  EXPECT_FALSE(result.accepted);
  EXPECT_FALSE(result.materialized);
  EXPECT_FALSE(error.empty() && result.error.empty());

  EXPECT_EQ(project_.CountStoredCommits(test::EditorMiniGitProjectFixture::kElementA), 0u);
  const auto remaining =
      project_.ReadJournalRecords(test::EditorMiniGitProjectFixture::kElementA, &error);
  EXPECT_EQ(remaining.size(), 1u) << error;
}

INSTANTIATE_TEST_SUITE_P(
    CaptureIdentityAndRange, EditorSaveCheckpointCaptureMismatchTest,
    ::testing::Values(CaptureMismatchKind::ElementId, CaptureMismatchKind::VersionId,
                      CaptureMismatchKind::RootId, CaptureMismatchKind::SequenceRange),
    [](const ::testing::TestParamInfo<CaptureMismatchKind>& info) {
      switch (info.param) {
        case CaptureMismatchKind::ElementId:
          return "ElementId";
        case CaptureMismatchKind::VersionId:
          return "VersionId";
        case CaptureMismatchKind::RootId:
          return "RootId";
        case CaptureMismatchKind::SequenceRange:
          return "SequenceRange";
      }
      return "Unknown";
    });

TEST_F(EditorSaveCheckpointCaptureTest,
       SameSessionSecondCaptureAfterSuccessfulMaterializeDoesNotReMaterializeAlreadySavedPrefix) {
  ASSERT_TRUE(
      project_.AppendExposureEdit(test::EditorMiniGitProjectFixture::kElementA, 0.0f, 0.6f));
  auto first = project_.CaptureWorkingState(test::EditorMiniGitProjectFixture::kElementA, 0.6f);
  ASSERT_TRUE(first.has_journal_range());
  const auto first_last = *first.last_journal_sequence;

  std::string error;
  ASSERT_TRUE(project_.MaterializeUnderSaveLock(first, &error).accepted) << error;

  // Live journal must match durable truncate without reopening the project.
  const auto live_after =
      project_.journal(test::EditorMiniGitProjectFixture::kElementA).Snapshot();
  EXPECT_TRUE(live_after.records.empty());
  EXPECT_FALSE(live_after.first_sequence.has_value());

  auto second = project_.CaptureWorkingState(test::EditorMiniGitProjectFixture::kElementA, 0.6f);
  EXPECT_TRUE(second.journal_records.empty());
  EXPECT_FALSE(second.has_journal_range());

  ASSERT_TRUE(
      project_.AppendExposureEdit(test::EditorMiniGitProjectFixture::kElementA, 0.6f, 1.1f));
  auto third = project_.CaptureWorkingState(test::EditorMiniGitProjectFixture::kElementA, 1.1f);
  ASSERT_TRUE(third.has_journal_range());
  EXPECT_EQ(third.journal_records.size(), 1u);
  EXPECT_GT(*third.first_journal_sequence, first_last);
  EXPECT_EQ(*third.first_journal_sequence, *third.last_journal_sequence);

  ASSERT_TRUE(project_.MaterializeUnderSaveLock(third, &error).accepted) << error;
  EXPECT_EQ(project_.CountStoredCommits(test::EditorMiniGitProjectFixture::kElementA), 2u);
  EXPECT_TRUE(
      project_.ReadJournalRecords(test::EditorMiniGitProjectFixture::kElementA, &error).empty())
      << error;
  EXPECT_TRUE(
      project_.journal(test::EditorMiniGitProjectFixture::kElementA).Snapshot().records.empty());
}

TEST_F(EditorSaveCheckpointCaptureTest, EmptyJournalWithClaimedSequenceRangeStartsNoMaterialization) {
  auto capture = project_.CaptureWorkingState(test::EditorMiniGitProjectFixture::kElementA, 0.0f);
  ASSERT_TRUE(capture.journal_records.empty());
  capture.first_journal_sequence = 1;
  capture.last_journal_sequence  = 1;

  std::string error;
  const auto  result = project_.MaterializeUnderSaveLock(capture, &error);
  EXPECT_FALSE(result.accepted);
  EXPECT_FALSE(result.materialized);
  EXPECT_EQ(project_.CountStoredCommits(test::EditorMiniGitProjectFixture::kElementA), 0u);
}

TEST_F(EditorSaveCheckpointCaptureTest, NonContiguousJournalSequencesStartNoMaterialization) {
  ASSERT_TRUE(
      project_.AppendExposureEdit(test::EditorMiniGitProjectFixture::kElementA, 0.0f, 0.3f));
  ASSERT_TRUE(
      project_.AppendExposureEdit(test::EditorMiniGitProjectFixture::kElementA, 0.3f, 0.7f));
  auto capture = project_.CaptureWorkingState(test::EditorMiniGitProjectFixture::kElementA, 0.7f);
  ASSERT_EQ(capture.journal_records.size(), 2u);
  // Keep bounds aligned with front/back while inserting a hole so validation
  // fails on contiguity, not on range identity.
  capture.journal_records.back().sequence = capture.journal_records.front().sequence + 2;
  capture.last_journal_sequence           = capture.journal_records.back().sequence;

  std::string error;
  const auto  result = project_.MaterializeUnderSaveLock(capture, &error);
  EXPECT_FALSE(result.accepted);
  EXPECT_FALSE(result.materialized);
  EXPECT_EQ(project_.CountStoredCommits(test::EditorMiniGitProjectFixture::kElementA), 0u);
  EXPECT_EQ(
      project_.ReadJournalRecords(test::EditorMiniGitProjectFixture::kElementA, &error).size(), 2u)
      << error;
}

TEST_F(EditorSaveCheckpointCaptureTest, FirstSequenceGreaterThanLastStartsNoMaterialization) {
  ASSERT_TRUE(
      project_.AppendExposureEdit(test::EditorMiniGitProjectFixture::kElementA, 0.0f, 0.4f));
  auto capture = project_.CaptureWorkingState(test::EditorMiniGitProjectFixture::kElementA, 0.4f);
  ASSERT_TRUE(capture.has_journal_range());
  capture.first_journal_sequence = 9;
  capture.last_journal_sequence  = 1;
  // Keep record sequences matching the (invalid) bounds so validation fails on
  // range order rather than front/back identity first.
  capture.journal_records.front().sequence = 9;

  std::string error;
  const auto  result = project_.MaterializeUnderSaveLock(capture, &error);
  EXPECT_FALSE(result.accepted);
  EXPECT_FALSE(result.materialized);
  EXPECT_EQ(project_.CountStoredCommits(test::EditorMiniGitProjectFixture::kElementA), 0u);
}

TEST(MiniGitJournalConcurrency, ConcurrentAppendDuringSnapshotNeverObservesTornRecordList) {
  const auto stamp =
      std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
  const auto path =
      std::filesystem::temp_directory_path() / ("mini_git_journal_concurrent_" + stamp + ".wal");
  std::error_code remove_ec;
  std::filesystem::remove(path, remove_ec);

  MiniGitJournal journal(path);
  constexpr int  kAppends = 200;

  std::mutex              gate_mutex;
  std::condition_variable gate;
  int                     ready = 0;
  bool                    go    = false;
  auto                    wait_for_start = [&] {
    std::unique_lock lock(gate_mutex);
    ++ready;
    gate.notify_all();
    gate.wait(lock, [&] { return go; });
  };

  std::vector<MiniGitJournalSnapshot> snapshots;
  snapshots.reserve(static_cast<std::size_t>(kAppends));
  std::mutex snapshots_mutex;

  std::thread appender([&] {
    wait_for_start();
    for (int i = 0; i < kAppends; ++i) {
      MiniGitJournalRecord record =
          MakeHeadMoveRecord(std::nullopt, Hash128{}, std::nullopt, Hash128{});
      std::string error;
      ASSERT_TRUE(journal.Append(record, &error)) << error;
    }
  });

  std::thread snapshotter([&] {
    wait_for_start();
    for (int i = 0; i < kAppends; ++i) {
      auto snap = journal.Snapshot();
      std::scoped_lock lock(snapshots_mutex);
      snapshots.push_back(std::move(snap));
    }
  });

  {
    std::unique_lock lock(gate_mutex);
    gate.wait(lock, [&] { return ready == 2; });
    go = true;
  }
  gate.notify_all();
  appender.join();
  snapshotter.join();

  for (const auto& snap : snapshots) {
    if (snap.records.empty()) {
      EXPECT_FALSE(snap.first_sequence.has_value());
      EXPECT_FALSE(snap.last_sequence.has_value());
      continue;
    }
    ASSERT_TRUE(snap.first_sequence.has_value());
    ASSERT_TRUE(snap.last_sequence.has_value());
    EXPECT_EQ(snap.records.front().sequence, *snap.first_sequence);
    EXPECT_EQ(snap.records.back().sequence, *snap.last_sequence);
    for (std::size_t i = 0; i < snap.records.size(); ++i) {
      EXPECT_EQ(snap.records[i].sequence, *snap.first_sequence + i);
    }
    EXPECT_EQ(snap.records.size(),
              static_cast<std::size_t>(*snap.last_sequence - *snap.first_sequence + 1));
  }

  const auto final_snap = journal.Snapshot();
  ASSERT_EQ(final_snap.records.size(), static_cast<std::size_t>(kAppends));
  EXPECT_EQ(*final_snap.first_sequence, 1u);
  EXPECT_EQ(*final_snap.last_sequence, static_cast<std::uint64_t>(kAppends));

  std::filesystem::remove(path, remove_ec);
}

}  // namespace
}  // namespace alcedo
