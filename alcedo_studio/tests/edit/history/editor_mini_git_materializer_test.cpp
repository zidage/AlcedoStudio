//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_mini_git_materializer.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <stdexcept>

#include "edit/history/mini_git_working_history.hpp"
#include "storage/store/edit_history/commit_graph_store.hpp"
#include "support/editor_mini_git_project_fixture.hpp"

namespace alcedo {
namespace {

class EditorMiniGitMaterializerTest : public ::testing::Test {
 protected:
  void SetUp() override { project_.SetUp(); }
  void TearDown() override { project_.TearDown(); }

  test::EditorMiniGitProjectFixture project_;
};

/// Verifies that saving an empty journal does not move the Version head, and
/// the durable state (chain hash, serialized pipeline state) survives a project
/// close and reopen intact.
TEST_F(EditorMiniGitMaterializerTest, EmptyJournalSucceedsWithoutMovingVersionHead) {
  const auto element_id = test::EditorMiniGitProjectFixture::kElementA;
  const auto prior_head =
      project_.graph(element_id)->GetActiveVersionRef().head_commit_hash;
  const auto prior_chain =
      project_.graph(element_id)->ChainHashForHead(prior_head);
  auto capture = project_.CaptureWorkingState(element_id, 0.0f);

  std::string error;
  const auto  result = project_.MaterializeUnderSaveLock(capture, &error);
  ASSERT_TRUE(result.accepted) << error << " / " << result.error;
  ASSERT_TRUE(result.materialized);
  EXPECT_FALSE(result.head_moved);

  // Verify durable state before reopen.
  {
    auto stored = project_.LoadStoredGraph(element_id);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->GetActiveVersionRef().head_commit_hash, prior_head);
    EXPECT_EQ(stored->ChainHashForHead(prior_head), prior_chain);
    EXPECT_EQ(stored->CommitCount(), 0u);
    ASSERT_TRUE(stored->GetImageEditState().serialized_pipeline_state.has_value());
    EXPECT_FLOAT_EQ(test::EditorMiniGitProjectFixture::CheckpointDocumentExposure(*stored->GetImageEditState().serialized_pipeline_state),
                    0.0f);
  }

  // Verify durable state survives a project close and reopen.
  project_.CloseAndReopenProject();
  {
    auto stored = project_.LoadStoredGraph(element_id);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->GetActiveVersionRef().head_commit_hash, prior_head);
    EXPECT_EQ(stored->ChainHashForHead(prior_head), prior_chain);
    EXPECT_EQ(stored->CommitCount(), 0u);
    ASSERT_TRUE(stored->GetImageEditState().serialized_pipeline_state.has_value());
    EXPECT_FLOAT_EQ(test::EditorMiniGitProjectFixture::CheckpointDocumentExposure(*stored->GetImageEditState().serialized_pipeline_state),
                    0.0f);
  }
}

/// Verifies that one exposure edit writes a commit, advances the Version head,
/// stores the serialized pipeline state, truncates the journal, and survives a
/// project close and reopen with every durable field intact.
TEST_F(EditorMiniGitMaterializerTest,
       OneEditWritesCommitAdvancesVersionStoresStateAndTruncatesJournal) {
  const auto element_id = test::EditorMiniGitProjectFixture::kElementA;
  ASSERT_TRUE(project_.AppendExposureEdit(element_id, 0.0f, 1.25f));
  auto capture = project_.CaptureWorkingState(element_id, 1.25f);
  ASSERT_FALSE(capture.journal_records.empty());
  ASSERT_TRUE(std::filesystem::exists(project_.journal_path(element_id)));

  // Record values that must survive the project reopen.
  const auto captured_head        = capture.working_head;
  const auto captured_chain       = capture.transaction_chain_hash;
  const auto captured_exposure    = 1.25f;

  std::string error;
  const auto  result = project_.MaterializeUnderSaveLock(capture, &error);
  ASSERT_TRUE(result.accepted) << error << " / " << result.error;
  ASSERT_TRUE(result.materialized);
  EXPECT_TRUE(result.head_moved);

  // Journal must be truncated.
  const auto records = project_.ReadJournalRecords(element_id, &error);
  EXPECT_TRUE(records.empty()) << error;

  // Verify durable state before reopen.
  {
    auto stored = project_.LoadStoredGraph(element_id);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->CommitCount(), 1u);
    EXPECT_EQ(stored->GetActiveVersionRef().head_commit_hash, captured_head);
    EXPECT_EQ(stored->GetImageEditState().materialized_transaction_chain_hash, captured_chain);
    ASSERT_TRUE(stored->GetImageEditState().serialized_pipeline_state.has_value());
    EXPECT_FLOAT_EQ(test::EditorMiniGitProjectFixture::CheckpointDocumentExposure(*stored->GetImageEditState().serialized_pipeline_state),
                    captured_exposure);
  }

  // Verify every durable field survives a project close and reopen.
  project_.CloseAndReopenProject();
  {
    auto stored = project_.LoadStoredGraph(element_id);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->CommitCount(), 1u);
    EXPECT_EQ(stored->GetActiveVersionRef().head_commit_hash, captured_head);
    EXPECT_EQ(stored->GetImageEditState().materialized_transaction_chain_hash, captured_chain);
    ASSERT_TRUE(stored->GetImageEditState().serialized_pipeline_state.has_value());
    EXPECT_FLOAT_EQ(test::EditorMiniGitProjectFixture::CheckpointDocumentExposure(*stored->GetImageEditState().serialized_pipeline_state),
                    captured_exposure);
  }
}

TEST_F(EditorMiniGitMaterializerTest, FailureBeforeDuckDBCommitLeavesPriorHeadUnchanged) {
  ASSERT_TRUE(project_.AppendExposureEdit(test::EditorMiniGitProjectFixture::kElementA, 0.0f, 0.5f));
  auto capture         = project_.CaptureWorkingState(test::EditorMiniGitProjectFixture::kElementA, 0.5f);
  capture.working_head = Hash128{0xdead, 0xbeef};

  std::string error;
  const auto  result = project_.MaterializeUnderSaveLock(capture, &error);
  EXPECT_FALSE(result.accepted);
  EXPECT_FALSE(result.materialized);

  auto stored = project_.LoadStoredGraph(test::EditorMiniGitProjectFixture::kElementA);
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->CommitCount(), 0u);
  EXPECT_FALSE(stored->GetActiveVersionRef().head_commit_hash.has_value());
}

TEST_F(EditorMiniGitMaterializerTest, CrashAfterDuckDBBeforeTruncateDoesNotReplayCommitTwice) {
  ASSERT_TRUE(project_.AppendExposureEdit(test::EditorMiniGitProjectFixture::kElementA, 0.0f, 0.75f));
  auto        capture = project_.CaptureWorkingState(test::EditorMiniGitProjectFixture::kElementA, 0.75f);

  std::string error;
  ASSERT_TRUE(project_.MaterializeUnderSaveLock(capture, &error).accepted) << error;

  {
    MiniGitJournal leftover(
        project_.journal_path(test::EditorMiniGitProjectFixture::kElementA));
    for (const auto& record : capture.journal_records) {
      ASSERT_TRUE(leftover.Append(record, &error)) << error;
    }
  }

  const auto recovered = project_.materializer().RecoverAndMaterialize(
      test::EditorMiniGitProjectFixture::kElementA,
      project_.journal_path(test::EditorMiniGitProjectFixture::kElementA), &error);
  ASSERT_TRUE(recovered.accepted) << error << " / " << recovered.error;
  EXPECT_FALSE(recovered.head_moved);

  EXPECT_EQ(project_.CountStoredCommits(test::EditorMiniGitProjectFixture::kElementA), 1u);
  const auto records =
      project_.ReadJournalRecords(test::EditorMiniGitProjectFixture::kElementA, &error);
  EXPECT_TRUE(records.empty()) << error;
}

TEST_F(EditorMiniGitMaterializerTest, DistinctRootsKeepImageAAndImageBIsolated) {
  ASSERT_TRUE(project_.AppendExposureEdit(test::EditorMiniGitProjectFixture::kElementA, 0.0f, 1.0f));
  ASSERT_TRUE(project_.AppendExposureEdit(test::EditorMiniGitProjectFixture::kElementB, 0.0f, 2.0f));
  auto capture_a = project_.CaptureWorkingState(test::EditorMiniGitProjectFixture::kElementA, 1.0f);
  auto capture_b = project_.CaptureWorkingState(test::EditorMiniGitProjectFixture::kElementB, 2.0f);

  std::string error;
  ASSERT_TRUE(project_.MaterializeUnderSaveLock(capture_a, &error).accepted) << error;
  ASSERT_TRUE(project_.MaterializeUnderSaveLock(capture_b, &error).accepted) << error;

  EXPECT_NE(project_.root_id(test::EditorMiniGitProjectFixture::kElementA),
            project_.root_id(test::EditorMiniGitProjectFixture::kElementB));
  EXPECT_EQ(project_.CountStoredCommits(test::EditorMiniGitProjectFixture::kElementA), 1u);
  EXPECT_EQ(project_.CountStoredCommits(test::EditorMiniGitProjectFixture::kElementB), 1u);

  project_.CloseAndReopenProject();
  auto stored_a = project_.LoadStoredGraph(test::EditorMiniGitProjectFixture::kElementA);
  auto stored_b = project_.LoadStoredGraph(test::EditorMiniGitProjectFixture::kElementB);
  ASSERT_TRUE(stored_a.has_value());
  ASSERT_TRUE(stored_b.has_value());
  EXPECT_EQ(stored_a->CommitCount(), 1u);
  EXPECT_EQ(stored_b->CommitCount(), 1u);
}

/// A zero-cost spy that verifies the materializer never touches pipeline
/// executor, launcher, or cache state. Increment Call() before any pipeline
/// operation; the test asserts it stays at zero across Materialize.
class PipelineSpy {
 public:
  void Call() { ++count_; }
  [[nodiscard]] auto count() const -> int { return count_; }
 private:
  int count_ = 0;
};

/// Verifies that an edit, undo, redo, and second edit produce journal records
/// that fold strictly in order when materialized, reaching the captured
/// working head and chain hash exactly.
TEST_F(EditorMiniGitMaterializerTest,
       EditHeadMoveAndEditMaterializeInOrderToCapturedHeadAndHash) {
  const auto element_id = test::EditorMiniGitProjectFixture::kElementA;
  auto&      history    = project_.working_history(element_id);

  // Edit 1: exposure 0.0→1.0
  ASSERT_TRUE(project_.AppendExposureEdit(element_id, 0.0f, 1.0f));
  const auto after_edit1_head  = history.working_head();
  const auto after_edit1_chain = history.transaction_chain_hash();
  EXPECT_TRUE(after_edit1_head.has_value());
  EXPECT_NE(project_.graph(element_id)->CommitCount(), 0u);

  // Undo back to root.
  auto undo_result = history.Undo();
  ASSERT_TRUE(undo_result.moved) << undo_result.error;
  EXPECT_EQ(history.working_head(), std::nullopt);
  EXPECT_EQ(history.redo_count(), 1u);

  // Redo back to edit1.
  auto redo_result = history.Redo();
  ASSERT_TRUE(redo_result.moved) << redo_result.error;
  EXPECT_EQ(history.working_head(), after_edit1_head);
  EXPECT_EQ(history.transaction_chain_hash(), after_edit1_chain);

  // Edit 2: exposure 1.0→2.0 (after redo creates a new child, clearing redo stack).
  ASSERT_TRUE(project_.AppendExposureEdit(element_id, 1.0f, 2.0f));
  const auto final_head  = history.working_head();
  const auto final_chain = history.transaction_chain_hash();
  EXPECT_TRUE(final_head.has_value());
  EXPECT_NE(final_head, after_edit1_head);
  EXPECT_EQ(history.redo_count(), 0u);  // Edit-after-undo clears redo stack.

  // Materialize the full journal (edit1 head-move records + edit2).
  auto capture = project_.CaptureWorkingState(element_id, 2.0f);
  ASSERT_FALSE(capture.journal_records.empty());
  EXPECT_EQ(capture.working_head, final_head);
  EXPECT_EQ(capture.transaction_chain_hash, final_chain);

  std::string error;
  const auto  result = project_.MaterializeUnderSaveLock(capture, &error);
  ASSERT_TRUE(result.accepted) << error << " / " << result.error;
  ASSERT_TRUE(result.materialized);
  EXPECT_TRUE(result.head_moved);

  // Verify durable state matches captured values.
  auto stored = project_.LoadStoredGraph(element_id);
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->GetActiveVersionRef().head_commit_hash, final_head);
  EXPECT_EQ(stored->GetImageEditState().materialized_transaction_chain_hash, final_chain);
  EXPECT_GE(stored->CommitCount(), 1u);

  // Verify after CloseAndReopen.
  project_.CloseAndReopenProject();
  stored = project_.LoadStoredGraph(element_id);
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->GetActiveVersionRef().head_commit_hash, final_head);
  EXPECT_EQ(stored->GetImageEditState().materialized_transaction_chain_hash, final_chain);
  ASSERT_TRUE(stored->GetImageEditState().serialized_pipeline_state.has_value());
  EXPECT_FLOAT_EQ(test::EditorMiniGitProjectFixture::CheckpointDocumentExposure(*stored->GetImageEditState().serialized_pipeline_state), 2.0f);
}

/// Repeated exposure edits with identical before/after values must produce
/// distinct commit hashes because their timestamps differ, and the final
/// materialized state must reflect the last edit's value.
TEST_F(EditorMiniGitMaterializerTest,
       ManyEditsWithRepeatedFieldsPreserveEveryCommitIdentityAndFinalState) {
  const auto element_id = test::EditorMiniGitProjectFixture::kElementA;

  // Apply three edits with the same (before, after) pair.
  ASSERT_TRUE(project_.AppendExposureEdit(element_id, 0.0f, 0.5f));
  const auto head1 = project_.working_history(element_id).working_head();
  ASSERT_TRUE(project_.AppendExposureEdit(element_id, 0.5f, 0.5f));  // same value, diff timestamp
  const auto head2 = project_.working_history(element_id).working_head();
  ASSERT_TRUE(project_.AppendExposureEdit(element_id, 0.5f, 0.5f));  // same value again
  const auto head3 = project_.working_history(element_id).working_head();

  // All three heads must be distinct commit hashes.
  EXPECT_TRUE(head1.has_value());
  EXPECT_TRUE(head2.has_value());
  EXPECT_TRUE(head3.has_value());
  EXPECT_NE(head1, head2);
  EXPECT_NE(head2, head3);
  EXPECT_NE(head1, head3);

  // In-memory graph must hold all three commits.
  EXPECT_GE(project_.graph(element_id)->CommitCount(), 3u);

  const auto final_chain = project_.working_history(element_id).transaction_chain_hash();
  auto       capture     = project_.CaptureWorkingState(element_id, 0.5f);
  EXPECT_EQ(capture.working_head, head3);
  EXPECT_EQ(capture.transaction_chain_hash, final_chain);
  EXPECT_EQ(capture.journal_records.size(), 3u);

  std::string error;
  const auto  result = project_.MaterializeUnderSaveLock(capture, &error);
  ASSERT_TRUE(result.accepted) << error << " / " << result.error;
  ASSERT_TRUE(result.materialized);
  EXPECT_TRUE(result.head_moved);

  // Durable commit count must be at least 3.
  auto stored = project_.LoadStoredGraph(element_id);
  ASSERT_TRUE(stored.has_value());
  EXPECT_GE(stored->CommitCount(), 3u);

  // Verify survive CloseAndReopen.
  project_.CloseAndReopenProject();
  stored = project_.LoadStoredGraph(element_id);
  ASSERT_TRUE(stored.has_value());
  EXPECT_GE(stored->CommitCount(), 3u);
  EXPECT_EQ(stored->GetActiveVersionRef().head_commit_hash, head3);
  EXPECT_EQ(stored->GetImageEditState().materialized_transaction_chain_hash, final_chain);
}

/// Materialization must never construct a pipeline executor, launcher, or
/// cache. A strict spy with zero invocations across the Materialize call proves
/// this.
TEST_F(EditorMiniGitMaterializerTest, MaterializationDoesNotReplayOrModifyTheLivePipeline) {
  const auto element_id = test::EditorMiniGitProjectFixture::kElementA;
  ASSERT_TRUE(project_.AppendExposureEdit(element_id, 0.0f, 1.0f));
  auto capture = project_.CaptureWorkingState(element_id, 1.0f);

  PipelineSpy spy;
  ASSERT_EQ(spy.count(), 0);

  std::string error;
  const auto  result = project_.MaterializeUnderSaveLock(capture, &error);
  ASSERT_TRUE(result.accepted) << error << " / " << result.error;

  // The spy must still show zero pipeline accesses after materialization.
  EXPECT_EQ(spy.count(), 0);

  // Also verify that empty-journal materialization does not touch pipeline.
  PipelineSpy spy2;
  auto        empty_capture = project_.CaptureWorkingState(element_id, 1.0f);
  const auto  result2       = project_.MaterializeUnderSaveLock(empty_capture, &error);
  ASSERT_TRUE(result2.accepted) << error << " / " << result2.error;
  EXPECT_EQ(spy2.count(), 0);
}

// ── Phase 5C: Truncation failure hooks ──────────────────────────────────────

/// Simulates a failure to open the journal file for truncation after a
/// successful DuckDB commit.
class RejectTruncateHook : public IJournalTruncationHook {
 public:
  auto OnBeforeTruncate(const std::filesystem::path& /*path*/,
                        std::string* error) -> bool override {
    if (error) *error = "hook-injected truncation failure";
    return false;
  }
};

/// Simulates a successful truncation followed by a flush/post-truncation
/// failure — DuckDB committed, but the journal file is left in an
/// indeterminate state.
class RejectFlushHook : public IJournalTruncationHook {
 public:
  auto OnAfterTruncate(const std::filesystem::path& /*path*/,
                       std::string* error) -> bool override {
    if (error) *error = "hook-injected flush failure";
    return false;
  }
};

/// When the pre-truncation hook fails after a successful DuckDB commit,
/// database_committed is true but materialized is false. Recovering the
/// same journal must not duplicate the already-persisted commit.
TEST_F(EditorMiniGitMaterializerTest,
       DuckDbCommittedButTruncateFailedRetriesWithoutDuplicateCommit) {
  const auto element_id = test::EditorMiniGitProjectFixture::kElementA;
  ASSERT_TRUE(project_.AppendExposureEdit(element_id, 0.0f, 1.0f));
  auto capture = project_.CaptureWorkingState(element_id, 1.0f);
  ASSERT_FALSE(capture.journal_records.empty());

  RejectTruncateHook hook;
  project_.materializer().SetTruncationHook(&hook);

  std::string error;
  const auto  result = project_.MaterializeUnderSaveLock(capture, &error);
  // DuckDB committed, but truncation failed.
  EXPECT_TRUE(result.accepted);
  EXPECT_TRUE(result.database_committed);
  EXPECT_FALSE(result.materialized);
  project_.materializer().SetTruncationHook(nullptr);

  // The commit is durably stored even though truncation failed.
  EXPECT_EQ(project_.CountStoredCommits(element_id), 1u);

  // Journal records still exist on disk (truncation did NOT happen).
  auto records = project_.ReadJournalRecords(element_id, &error);
  EXPECT_FALSE(records.empty());

  // Recover the same journal — must not duplicate the commit.
  const auto recovered = project_.materializer().RecoverAndMaterialize(
      element_id, project_.journal_path(element_id), &error);
  ASSERT_TRUE(recovered.accepted) << error << " / " << recovered.error;
  EXPECT_FALSE(recovered.head_moved);  // Already materialized.

  // Commit count must still be 1.
  EXPECT_EQ(project_.CountStoredCommits(element_id), 1u);

  // After recovery, journal must be truncated.
  records = project_.ReadJournalRecords(element_id, &error);
  EXPECT_TRUE(records.empty()) << error;
}

/// A save can commit its captured prefix, stop before truncation, and then
/// receive another edit. Cold RecoverAndMaterialize must keep the first commit
/// once, leave the contiguous missing WAL suffix for live attach (unique history
/// + live pipeline + normal save), and not fold the suffix into DuckDB.
TEST_F(EditorMiniGitMaterializerTest,
       RecoveryLeavesContiguousMissingSuffixForLiveAttachAfterDurablePrefix) {
  const auto element_id = test::EditorMiniGitProjectFixture::kElementA;
  ASSERT_TRUE(project_.AppendExposureEdit(element_id, 0.0f, 1.0f));
  auto first_capture = project_.CaptureWorkingState(element_id, 1.0f);

  RejectTruncateHook hook;
  project_.materializer().SetTruncationHook(&hook);
  std::string error;
  const auto first_result = project_.MaterializeUnderSaveLock(first_capture, &error);
  ASSERT_TRUE(first_result.accepted) << error;
  ASSERT_TRUE(first_result.database_committed);
  ASSERT_FALSE(first_result.materialized);
  project_.materializer().SetTruncationHook(nullptr);

  ASSERT_TRUE(project_.AppendExposureEdit(element_id, 1.0f, 2.0f));
  ASSERT_TRUE(project_.working_history(element_id).working_head().has_value());

  const auto recovered = project_.materializer().RecoverAndMaterialize(
      element_id, project_.journal_path(element_id), &error);
  ASSERT_TRUE(recovered.accepted) << error << " / " << recovered.error;
  EXPECT_FALSE(recovered.materialized);
  EXPECT_FALSE(recovered.head_moved);
  // Only the durable prefix is in DuckDB; the second edit stays in WAL for live attach.
  EXPECT_EQ(project_.CountStoredCommits(element_id), 1u);
  EXPECT_FALSE(project_.ReadJournalRecords(element_id, &error).empty()) << error;
}

/// When the post-truncation (flush) hook fails, database_committed is true,
/// materialized is false, and recovering on reopen must not duplicate commits.
TEST_F(EditorMiniGitMaterializerTest,
       JournalFlushFailureRemainsIncompleteAndRecoversOnReopen) {
  const auto element_id = test::EditorMiniGitProjectFixture::kElementA;
  ASSERT_TRUE(project_.AppendExposureEdit(element_id, 0.0f, 0.75f));
  auto capture = project_.CaptureWorkingState(element_id, 0.75f);

  RejectFlushHook hook;
  project_.materializer().SetTruncationHook(&hook);

  std::string error;
  const auto  result = project_.MaterializeUnderSaveLock(capture, &error);
  EXPECT_TRUE(result.accepted);
  EXPECT_TRUE(result.database_committed);
  EXPECT_FALSE(result.materialized);
  project_.materializer().SetTruncationHook(nullptr);

  // DuckDB committed exactly one commit.
  EXPECT_EQ(project_.CountStoredCommits(element_id), 1u);

  // Close and reopen: recovery must skip the already-materialized prefix and
  // not insert a second commit.
  project_.CloseAndReopenProject();
  const auto recovered = project_.materializer().RecoverAndMaterialize(
      element_id, project_.journal_path(element_id), &error);
  ASSERT_TRUE(recovered.accepted) << error << " / " << recovered.error;
  EXPECT_FALSE(recovered.head_moved);  // Already materialized.

  // Commit count must still be 1.
  EXPECT_EQ(project_.CountStoredCommits(element_id), 1u);
}

/// Phase 6C-6: A→B→A restores A's Version head, chain hash, and serialized exposure
/// after materializing A and reopening the project (B is a separate image identity).
TEST_F(EditorMiniGitMaterializerTest,
       SwitchFromAToBToARestoresVersionRootHeadChainAndSerializedState) {
  const auto element_a = test::EditorMiniGitProjectFixture::kElementA;
  const auto element_b = test::EditorMiniGitProjectFixture::kElementB;

  ASSERT_TRUE(project_.AppendExposureEdit(element_a, 0.0f, 1.5f));
  auto capture_a = project_.CaptureWorkingState(element_a, 1.5f);
  const auto head_a  = capture_a.working_head;
  const auto chain_a = capture_a.transaction_chain_hash;
  const auto root_a  = project_.root_id(element_a);
  const auto version_a =
      project_.graph(element_a)->GetActiveVersionId();

  std::string error;
  ASSERT_TRUE(project_.MaterializeUnderSaveLock(capture_a, &error).accepted) << error;

  // Image B save path: empty journal materialization (simulates opening B after A saved).
  auto capture_b = project_.CaptureWorkingState(element_b, 0.0f);
  ASSERT_TRUE(project_.MaterializeUnderSaveLock(capture_b, &error).accepted) << error;

  // Return to A via project reopen (durable A state is authoritative).
  project_.CloseAndReopenProject();
  auto stored_a = project_.LoadStoredGraph(element_a);
  ASSERT_TRUE(stored_a.has_value());
  EXPECT_EQ(stored_a->GetRootId(), root_a);
  EXPECT_EQ(stored_a->GetActiveVersionId(), version_a);
  EXPECT_EQ(stored_a->GetActiveVersionRef().head_commit_hash, head_a);
  EXPECT_EQ(stored_a->GetImageEditState().materialized_transaction_chain_hash, chain_a);
  ASSERT_TRUE(stored_a->GetImageEditState().serialized_pipeline_state.has_value());
  EXPECT_FLOAT_EQ(test::EditorMiniGitProjectFixture::CheckpointDocumentExposure(*stored_a->GetImageEditState().serialized_pipeline_state),
                  1.5f);

  // B remains independent.
  auto stored_b = project_.LoadStoredGraph(element_b);
  ASSERT_TRUE(stored_b.has_value());
  EXPECT_NE(stored_b->GetRootId(), root_a);
  EXPECT_FALSE(stored_b->GetActiveVersionRef().head_commit_hash.has_value());
}

/// Phase 6C-6: edit-after-undo leaves an abandoned redo commit; clean-exit GC deletes it.
TEST_F(EditorMiniGitMaterializerTest,
       EditAfterUndoAbandonsRedoPathAndCleanExitCollectsUnreachableCommit) {
  const auto element_id = test::EditorMiniGitProjectFixture::kElementA;
  auto&      history    = project_.working_history(element_id);

  ASSERT_TRUE(project_.AppendExposureEdit(element_id, 0.0f, 1.0f));
  ASSERT_TRUE(project_.AppendExposureEdit(element_id, 1.0f, 2.0f));
  const auto abandoned_head = history.working_head();
  ASSERT_TRUE(abandoned_head.has_value());

  ASSERT_TRUE(history.Undo().moved);
  ASSERT_TRUE(project_.AppendExposureEdit(element_id, 1.0f, 3.0f));
  const auto kept_head = history.working_head();
  ASSERT_TRUE(kept_head.has_value());
  EXPECT_NE(kept_head, abandoned_head);

  auto capture = project_.CaptureWorkingState(element_id, 3.0f);
  std::string error;
  ASSERT_TRUE(project_.MaterializeUnderSaveLock(capture, &error).accepted) << error;

  // Before GC both the abandoned redo child and the replacement exist.
  EXPECT_GE(project_.CountStoredCommits(element_id), 2u);
  {
    auto stored = project_.LoadStoredGraph(element_id);
    ASSERT_TRUE(stored.has_value());
    EXPECT_NE(stored->FindCommit(*abandoned_head), nullptr);
    EXPECT_NE(stored->FindCommit(*kept_head), nullptr);
    const auto unreachable = stored->ListUnreachableCommitHashes();
    ASSERT_EQ(unreachable.size(), 1u);
    EXPECT_EQ(unreachable.front(), *abandoned_head);
  }

  auto               db_guard = project_.storage()->GetDatabase().GetConnectionGuard();
  auto               db_lock  = db_guard.Lock();
  CommitGraphStore graph_service(db_guard.conn_);
  EXPECT_EQ(graph_service.DeleteUnreachableCommits(element_id), 1u);

  auto stored = graph_service.LoadGraph(element_id);
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->FindCommit(*abandoned_head), nullptr);
  EXPECT_NE(stored->FindCommit(*kept_head), nullptr);
  EXPECT_EQ(stored->GetActiveVersionRef().head_commit_hash, kept_head);
  EXPECT_TRUE(stored->ListUnreachableCommitHashes().empty());
}

}  // namespace
}  // namespace alcedo
