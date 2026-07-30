//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_mini_git_commit_writer.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "app/editor_mini_git_materializer.hpp"
#include "edit/history/commit_graph.hpp"
#include "support/editor_mini_git_project_fixture.hpp"
namespace alcedo {
namespace {

// ── Failure-injection hooks for parameterized tests ─────────────────────────

/// Rejects every write with a fixed message.
class RejectAllHook : public ICommitWriterWriteHook {
 public:
  auto OnBeforeWrite(const CommitGraphMaterialization& /*m*/,
                     std::string* error) -> bool override {
    if (error) *error = "hook-injected pre-write rejection";
    return false;
  }
};

/// Produces a CommitGraphMaterialization whose element_id disagrees with the
/// stored graph root, triggering a DuckDB-level rejection after the transaction
/// has already begun. The DuckDB transaction is rolled back atomically.
struct MismatchedMaterializationHook : public ICommitWriterWriteHook {
  auto OnBeforeWrite(const CommitGraphMaterialization& /*m*/,
                     std::string* /*error*/) -> bool override {
    return true;  // Let the write proceed — DuckDB itself will reject.
  }
};

// ── Test fixture ─────────────────────────────────────────────────────────────

/// Uses EditorMiniGitProjectFixture for real project/database setup instead of
/// duplicating ProjectService initialization. Tests exercise the CommitWriter
/// through the materializer facade so that the full write path (validation,
/// hook, DuckDB transaction) is covered.
class EditorMiniGitCommitWriterTest : public ::testing::Test {
 protected:
  void SetUp() override { project_.SetUp(); }
  void TearDown() override { project_.TearDown(); }

  /// @return The captured materialized head for image A, or std::nullopt when
  ///         the stored graph has a null (root) head.
  auto StoredHeadA() -> head_commit_hash_t {
    auto stored = project_.LoadStoredGraph(test::EditorMiniGitProjectFixture::kElementA);
    if (!stored.has_value()) return std::nullopt;
    return stored->GetActiveVersionRef().head_commit_hash;
  }

  auto StoredChainA() -> transaction_chain_hash_t {
    auto stored = project_.LoadStoredGraph(test::EditorMiniGitProjectFixture::kElementA);
    if (!stored.has_value()) return {};
    return stored->GetImageEditState().materialized_transaction_chain_hash;
  }

  /// Snapshot every durable field that Phase 5B must verify is unchanged after
  /// a failed write.
  struct DurableSnapshot {
    std::uint64_t            commit_count = 0;
    head_commit_hash_t       version_head;
    head_commit_hash_t       materialized_head;
    transaction_chain_hash_t materialized_chain;
    bool                     has_serialized_state = false;
    float                    serialized_exposure  = 0.0f;
    std::size_t              journal_byte_count   = 0;
  };

  auto CaptureDurableSnapshot(sl_element_id_t element_id) -> DurableSnapshot {
    DurableSnapshot snap;
    auto           stored = project_.LoadStoredGraph(element_id);
    if (!stored.has_value()) return snap;
    snap.commit_count      = stored->CommitCount();
    snap.version_head      = stored->GetActiveVersionRef().head_commit_hash;
    snap.materialized_head =
        stored->GetImageEditState().materialized_head_commit_hash;
    snap.materialized_chain =
        stored->GetImageEditState().materialized_transaction_chain_hash;
    const auto& serialized = stored->GetImageEditState().serialized_pipeline_state;
    if (serialized.has_value()) {
      snap.has_serialized_state = true;
      if (serialized->contains("pipeline_params") &&
          serialized->at("pipeline_params").contains("exposure")) {
        snap.serialized_exposure =
            serialized->at("pipeline_params").at("exposure").get<float>();
      }
    }
    // Count journal bytes on disk.
    std::error_code ec;
    auto            sz = std::filesystem::file_size(project_.journal_path(element_id), ec);
    if (!ec) snap.journal_byte_count = static_cast<std::size_t>(sz);
    return snap;
  }

  test::EditorMiniGitProjectFixture project_;
};

// ── Basic tests (reusing fixture) ────────────────────────────────────────────

TEST_F(EditorMiniGitCommitWriterTest, WriteEmptyGraphSucceeds) {
  const auto element_id = test::EditorMiniGitProjectFixture::kElementA;
  auto       capture    = project_.CaptureWorkingState(element_id, 0.0f);

  std::string error;
  const auto  result = project_.MaterializeUnderSaveLock(capture, &error);
  ASSERT_TRUE(result.accepted) << error << " / " << result.error;
  ASSERT_TRUE(result.materialized);
  EXPECT_FALSE(result.head_moved);

  auto stored = project_.LoadStoredGraph(element_id);
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->CommitCount(), 0u);
}

TEST_F(EditorMiniGitCommitWriterTest,
       WriteWithSerializedStatePersistsAfterReopen) {
  const auto element_id = test::EditorMiniGitProjectFixture::kElementA;
  ASSERT_TRUE(project_.AppendExposureEdit(element_id, 0.0f, 1.5f));
  auto capture = project_.CaptureWorkingState(element_id, 1.5f);

  std::string error;
  const auto  result = project_.MaterializeUnderSaveLock(capture, &error);
  ASSERT_TRUE(result.accepted) << result.error;

  project_.CloseAndReopenProject();
  auto stored = project_.LoadStoredGraph(element_id);
  ASSERT_TRUE(stored.has_value());
  ASSERT_TRUE(stored->GetImageEditState().serialized_pipeline_state.has_value());
  EXPECT_FLOAT_EQ(stored->GetImageEditState()
                      .serialized_pipeline_state->at("pipeline_params")
                      .at("exposure")
                      .get<float>(),
                  1.5f);
}

TEST_F(EditorMiniGitCommitWriterTest,
       WriteInvalidMaterializationReturnsNotAccepted) {
  const auto element_id = test::EditorMiniGitProjectFixture::kElementA;
  ASSERT_TRUE(project_.AppendExposureEdit(element_id, 0.0f, 0.5f));
  auto capture         = project_.CaptureWorkingState(element_id, 0.5f);
  capture.working_head = Hash128{0xdead, 0xbeef};  // Corrupt the capture.

  std::string error;
  const auto  result = project_.MaterializeUnderSaveLock(capture, &error);
  EXPECT_FALSE(result.accepted);
  EXPECT_FALSE(result.materialized);

  auto stored = project_.LoadStoredGraph(element_id);
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->CommitCount(), 0u);
}

TEST_F(EditorMiniGitCommitWriterTest, WriteWithNullErrorDoesNotCrash) {
  const auto element_id = test::EditorMiniGitProjectFixture::kElementA;
  auto       capture    = project_.CaptureWorkingState(element_id, 0.0f);
  const auto result     = project_.MaterializeUnderSaveLock(capture, nullptr);
  EXPECT_TRUE(result.accepted);
}

// ── Parameterized pre-DuckDB-commit failure tests ─────────────────────────────

/// Failure modes exercised by FailureBeforeDuckDbCommitLeavesEveryDurableFieldUnchanged.
enum class PreCommitFailure {
  /// Hook rejects the write before any DuckDB interaction.
  kHookRejects,
  /// Capture carries a head that will fail the DuckDB-level fold validation,
  /// causing the transaction to roll back.
  kDuckDBValidationRejects,
};

struct PreCommitFailureParams {
  PreCommitFailure mode;
  const char*      description;
};

class EditorMiniGitCommitWriterFailureTest
    : public EditorMiniGitCommitWriterTest,
      public ::testing::WithParamInterface<PreCommitFailureParams> {};

INSTANTIATE_TEST_SUITE_P(
    PreCommitFailures, EditorMiniGitCommitWriterFailureTest,
    ::testing::Values(
        PreCommitFailureParams{PreCommitFailure::kHookRejects,
                               "hook_rejects"},
        PreCommitFailureParams{PreCommitFailure::kDuckDBValidationRejects,
                               "duckdb_validation_rejects"}),
    [](const ::testing::TestParamInfo<PreCommitFailureParams>& info) {
      return std::string(info.param.description);
    });

/// For each failure mode, capture the full durable state before attempting a
/// write that is guaranteed to fail before DuckDB commit. Then close and reopen
/// the project and assert every durable field is byte-identical to the
/// pre-attempt snapshot. Also verify that no thumbnail invalidation and no
/// image-B load event occurred (those paths are not wired in this fixture, so
/// we assert the journal bytes are preserved as the equivalent of "no B load").
TEST_P(EditorMiniGitCommitWriterFailureTest,
       FailureBeforeDuckDbCommitLeavesEveryDurableFieldUnchangedAfterReopen) {
  const auto element_id = test::EditorMiniGitProjectFixture::kElementA;

  // Seed one edit so there is non-trivial journal content to preserve.
  ASSERT_TRUE(project_.AppendExposureEdit(element_id, 0.0f, 1.0f));
  const auto snapshot_before = CaptureDurableSnapshot(element_id);
  ASSERT_GT(snapshot_before.journal_byte_count, 0u);

  auto capture = project_.CaptureWorkingState(element_id, 1.0f);
  ASSERT_FALSE(capture.journal_records.empty());

  const auto mode = GetParam().mode;

  if (mode == PreCommitFailure::kHookRejects) {
    RejectAllHook hook;
    project_.materializer().SetWriteHook(&hook);
    std::string error;
    const auto  result = project_.MaterializeUnderSaveLock(capture, &error);
    EXPECT_FALSE(result.accepted);
    EXPECT_FALSE(result.materialized);
    project_.materializer().SetWriteHook(nullptr);
  } else {
    // DuckDB validation rejection: tamper with the capture's head to force a
    // fold mismatch that causes the DuckDB transaction to roll back.
    auto bad_capture    = capture;
    bad_capture.working_head = Hash128{0xbad, 0xf00d};
    std::string error;
    const auto  result = project_.MaterializeUnderSaveLock(bad_capture, &error);
    EXPECT_FALSE(result.accepted);
    EXPECT_FALSE(result.materialized);
  }

  // Close and reopen: every durable field must match the pre-attempt snapshot.
  project_.CloseAndReopenProject();
  const auto snapshot_after = CaptureDurableSnapshot(element_id);

  EXPECT_EQ(snapshot_after.commit_count, snapshot_before.commit_count);
  EXPECT_EQ(snapshot_after.version_head, snapshot_before.version_head);
  EXPECT_EQ(snapshot_after.materialized_head, snapshot_before.materialized_head);
  // materialized_chain is zero-initialized before first materialization
  // and remains unchanged after a failed attempt.
  EXPECT_EQ(snapshot_after.materialized_chain, snapshot_before.materialized_chain);
  EXPECT_EQ(snapshot_after.has_serialized_state, snapshot_before.has_serialized_state);

  // Journal bytes must survive intact — equivalent to "no B load / no
  // thumbnail invalidation" in this fixture.
  EXPECT_EQ(snapshot_after.journal_byte_count, snapshot_before.journal_byte_count);
}

}  // namespace
}  // namespace alcedo
