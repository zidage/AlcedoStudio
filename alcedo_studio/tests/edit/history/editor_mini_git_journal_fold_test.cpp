//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// Alignment and replay helpers that replaced production EditorMiniGitJournalFold.
/// Normal save no longer folds WAL into DuckDB; these pure helpers support crash
/// recovery classification and unique-history suffix application only.

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "edit/history/commit_graph.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "utils/clock/time_provider.hpp"

namespace alcedo {
namespace {

auto MakeExposureBatch(float before, float after) -> PipelineEditBatch {
  PipelineEditBatch batch;
  SetParameterChange change;
  change.target.owner_kind             = PipelineParameterOwnerKind::ColorGrade;
  change.target.node_id                = NodeId{"grade.primary"};
  change.target.adjustment_instance_id = AdjustmentInstanceId{"grade.primary.exposure"};
  change.target.field_key              = "exposure";
  change.before_value                  = nlohmann::json{{"exposure_ev", before}};
  change.after_value                   = nlohmann::json{{"exposure_ev", after}};
  change.before_enabled                = true;
  change.after_enabled                 = true;
  batch.operation_kind                 = PipelineEditOperationKind::SetParameter;
  batch.presentation_key               = "history.operation.set_parameter";
  batch.changes.push_back(std::move(change));
  return batch;
}

class MiniGitJournalAlignmentTest : public ::testing::Test {
 protected:
  void SetUp() override {
    TimeProvider::Refresh();
    RegisterAllOperators();
    graph_   = std::make_shared<CommitGraph>(CommitGraph::CreateEmpty(1, "Default"));
    journal_ = std::make_shared<MiniGitJournal>();
    history_ = std::make_unique<MiniGitWorkingHistory>(graph_, journal_);
  }

  std::shared_ptr<CommitGraph>             graph_;
  std::shared_ptr<MiniGitJournal>          journal_;
  std::unique_ptr<MiniGitWorkingHistory>   history_;
};

TEST_F(MiniGitJournalAlignmentTest, EmptyRecordsAreFullyCovered) {
  const auto alignment = MiniGitWorkingHistory::AlignJournalWithStoredHead(*graph_, {});
  EXPECT_TRUE(alignment.accepted);
  EXPECT_TRUE(alignment.fully_covered);
  EXPECT_FALSE(alignment.broken);
  EXPECT_FALSE(alignment.contiguous_extension);
}

TEST_F(MiniGitJournalAlignmentTest, JournalMatchingStoredHeadIsFullyCovered) {
  ASSERT_TRUE(history_->AppendEdit(MakeExposureBatch(0.0f, 0.5f)).committed);
  const auto records = journal_->records();
  ASSERT_FALSE(records.empty());

  const auto alignment = MiniGitWorkingHistory::AlignJournalWithStoredHead(*graph_, records);
  ASSERT_TRUE(alignment.accepted) << alignment.error;
  EXPECT_TRUE(alignment.fully_covered);
  EXPECT_EQ(alignment.missing_from_index, records.size());
}

TEST_F(MiniGitJournalAlignmentTest, ContiguousMissingSuffixIsReportedAsExtension) {
  ASSERT_TRUE(history_->AppendEdit(MakeExposureBatch(0.0f, 0.25f)).committed);
  ASSERT_TRUE(history_->AppendEdit(MakeExposureBatch(0.25f, 0.5f)).committed);
  const auto records = journal_->records();
  ASSERT_EQ(records.size(), 2u);

  // Durable graph has only the first commit (crash before second persisted).
  auto durable = *graph_;
  durable.MoveWorkingHead(durable.GetActiveVersionId(), records.front().target_head);
  durable.EraseUnreachableCommits(durable.ListUnreachableCommitHashes());
  ASSERT_EQ(durable.CommitCount(), 1u);

  const auto alignment = MiniGitWorkingHistory::AlignJournalWithStoredHead(durable, records);
  ASSERT_TRUE(alignment.accepted) << alignment.error;
  EXPECT_TRUE(alignment.contiguous_extension);
  EXPECT_FALSE(alignment.fully_covered);
  EXPECT_EQ(alignment.missing_from_index, 1u);
}

TEST_F(MiniGitJournalAlignmentTest, BrokenParentChainIsRejected) {
  ASSERT_TRUE(history_->AppendEdit(MakeExposureBatch(0.0f, 0.5f)).committed);
  auto records = journal_->records();
  ASSERT_EQ(records.size(), 1u);

  // Second record claims a parent that does not match the first record's target.
  MiniGitJournalRecord broken = records.front();
  broken.sequence             = records.front().sequence + 1;
  broken.expected_source_head = Hash128{0xbad, 0xc0de};
  broken.expected_source_chain_hash = Hash128{0xdead, 0xbeef};
  records.push_back(broken);

  auto durable = *graph_;
  durable.MoveWorkingHead(durable.GetActiveVersionId(), std::nullopt);
  durable.EraseUnreachableCommits(durable.ListUnreachableCommitHashes());

  const auto alignment = MiniGitWorkingHistory::AlignJournalWithStoredHead(durable, records);
  EXPECT_TRUE(alignment.broken);
  EXPECT_FALSE(alignment.accepted);
}

TEST_F(MiniGitJournalAlignmentTest, UnalignedFirstRecordIsBroken) {
  ASSERT_TRUE(history_->AppendEdit(MakeExposureBatch(0.0f, 0.5f)).committed);
  auto records = journal_->records();
  ASSERT_FALSE(records.empty());
  records.front().expected_source_head = Hash128{0xbad, 0x1};
  records.front().expected_source_chain_hash = Hash128{0xbad, 0x2};

  auto durable = *graph_;
  durable.MoveWorkingHead(durable.GetActiveVersionId(), std::nullopt);
  durable.EraseUnreachableCommits(durable.ListUnreachableCommitHashes());

  const auto alignment = MiniGitWorkingHistory::AlignJournalWithStoredHead(durable, records);
  EXPECT_TRUE(alignment.broken);
  EXPECT_FALSE(alignment.accepted);
}

TEST_F(MiniGitJournalAlignmentTest, ReplaySkippingMaterializedPrefixAppliesOnlyMissingSuffix) {
  ASSERT_TRUE(history_->AppendEdit(MakeExposureBatch(0.0f, 0.25f)).committed);
  ASSERT_TRUE(history_->AppendEdit(MakeExposureBatch(0.25f, 0.5f)).committed);
  const auto records = journal_->records();
  ASSERT_EQ(records.size(), 2u);

  auto only_first = *graph_;
  only_first.MoveWorkingHead(only_first.GetActiveVersionId(), records.front().target_head);
  only_first.EraseUnreachableCommits(only_first.ListUnreachableCommitHashes());
  ASSERT_EQ(only_first.CommitCount(), 1u);

  std::size_t applied_from = 0;
  std::string error;
  ASSERT_TRUE(MiniGitWorkingHistory::ReplaySkippingMaterializedPrefix(only_first, records,
                                                                      &applied_from, &error))
      << error;
  EXPECT_EQ(applied_from, 1u);
  EXPECT_EQ(only_first.GetActiveVersionRef().head_commit_hash, records.back().target_head);
  EXPECT_EQ(only_first.CommitCount(), 2u);
}

TEST_F(MiniGitJournalAlignmentTest, StaleSourceHeadWritesNothingOnReplay) {
  MiniGitJournalRecord stale;
  stale.kind                       = MiniGitJournalRecordKind::kEditCommit;
  stale.expected_source_head       = Hash128{0xdead, 0xbeef};
  stale.expected_source_chain_hash = Hash128{0xcafe, 0xbabe};
  stale.target_head                = Hash128{0x1, 0x2};
  stale.target_chain_hash          = Hash128{0x3, 0x4};

  auto        target = *graph_;
  std::string error;
  EXPECT_FALSE(MiniGitWorkingHistory::Replay(target, {stale}, &error));
  EXPECT_EQ(target.CommitCount(), 0u);
  EXPECT_FALSE(target.GetActiveVersionRef().head_commit_hash.has_value());
}

}  // namespace
}  // namespace alcedo
