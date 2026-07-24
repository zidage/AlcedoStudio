//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_mini_git_journal_fold.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "edit/history/commit_graph.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "utils/clock/time_provider.hpp"

namespace alcedo {
namespace {

auto MakeExposurePayload(float before, float after) -> OrdinaryEditPayload {
  OrdinaryEditPayload payload;
  payload.operator_type  = OperatorType::EXPOSURE;
  payload.stage_name     = PipelineStageName::Basic_Adjustment;
  payload.field_name     = "$operator_params";
  payload.before_value   = nlohmann::json{{"exposure", before}};
  payload.after_value    = nlohmann::json{{"exposure", after}};
  payload.before_enabled = true;
  payload.after_enabled  = true;
  return payload;
}

}  // namespace

class EditorMiniGitJournalFoldTest : public ::testing::Test {
 protected:
  void SetUp() override {
    TimeProvider::Refresh();
    RegisterAllOperators();
    graph_ = std::make_shared<CommitGraph>(CommitGraph::CreateEmpty(element_id_, "Default"));
  }

  sl_element_id_t              element_id_ = 42;
  std::shared_ptr<CommitGraph> graph_;
};

TEST_F(EditorMiniGitJournalFoldTest, FoldEmptyRecordsReturnsAccepted) {
  std::string error;
  auto        result = EditorMiniGitJournalFold::Fold(*graph_, {}, &error);
  EXPECT_TRUE(result.accepted) << result.error;
}

TEST_F(EditorMiniGitJournalFoldTest, FoldEditCommitAdvancesHeadAndChainHash) {
  auto                  journal = std::make_shared<MiniGitJournal>();
  MiniGitWorkingHistory history(graph_, journal);
  const auto            prior_head = history.working_head();
  ASSERT_TRUE(history.AppendEdit(MakeExposurePayload(0.0f, 1.0f)).committed);

  // Build a fold graph from the original (unmodified) state, then fold the same
  // records the working history just created.
  auto        fold_graph = *graph_;
  std::string error;
  auto        result = EditorMiniGitJournalFold::Fold(fold_graph, journal->records(), &error);
  ASSERT_TRUE(result.accepted) << result.error;
  EXPECT_NE(fold_graph.GetActiveVersionRef().head_commit_hash, prior_head);
  EXPECT_EQ(fold_graph.GetActiveVersionRef().head_commit_hash, history.working_head());
}

TEST_F(EditorMiniGitJournalFoldTest, FoldCorruptedRecordReturnsNotAccepted) {
  std::vector<MiniGitJournalRecord> records;
  MiniGitJournalRecord              record;
  record.kind                 = MiniGitJournalRecordKind::kEditCommit;
  record.expected_source_head = graph_->GetActiveVersionRef().head_commit_hash;
  record.expected_source_chain_hash =
      graph_->ChainHashForHead(graph_->GetActiveVersionRef().head_commit_hash);
  // target_head is std::nullopt (root) but missing required commit data
  record.target_head       = std::nullopt;
  record.target_chain_hash = graph_->ChainHashForHead(std::nullopt);
  record.edit_commit       = std::nullopt;  // Missing commit object
  records.push_back(std::move(record));

  std::string error;
  auto        result = EditorMiniGitJournalFold::Fold(*graph_, records, &error);
  EXPECT_FALSE(result.accepted);
  EXPECT_FALSE(result.error.empty());
}

TEST_F(EditorMiniGitJournalFoldTest, FoldWithNullErrorDoesNotCrash) {
  auto                  journal = std::make_shared<MiniGitJournal>();
  MiniGitWorkingHistory history(graph_, journal);
  ASSERT_TRUE(history.AppendEdit(MakeExposurePayload(0.0f, 1.0f)).committed);

  auto fold_graph = *graph_;
  // Null error pointer must not crash.
  auto result     = EditorMiniGitJournalFold::Fold(fold_graph, journal->records(), nullptr);
  EXPECT_TRUE(result.accepted);
}

/// A journal record whose expected_source_head is stale (does not match the
/// current graph head) must cause the fold to reject without modifying the
/// graph.
TEST_F(EditorMiniGitJournalFoldTest, StaleSourceHeadOrChainHashWritesNothing) {
  auto                  journal = std::make_shared<MiniGitJournal>();
  MiniGitWorkingHistory history(graph_, journal);
  ASSERT_TRUE(history.AppendEdit(MakeExposurePayload(0.0f, 1.0f)).committed);

  // Capture the fold graph before the fold attempt.
  auto        fold_graph    = *graph_;
  const auto  prior_head    = fold_graph.GetActiveVersionRef().head_commit_hash;
  const auto  prior_chain   = fold_graph.ChainHashForHead(prior_head);
  const auto  prior_commits = fold_graph.CommitCount();

  // Build a record with a stale expected_source_head: a hash that does not
  // match the graph's current head. The record is otherwise structurally valid.
  MiniGitJournalRecord stale_record;
  stale_record.kind                   = MiniGitJournalRecordKind::kEditCommit;
  stale_record.expected_source_head   = Hash128{0xdead, 0xbeef};  // Stale.
  stale_record.expected_source_chain_hash = prior_chain;
  stale_record.edit_commit            = journal->records().front().edit_commit;
  stale_record.target_head            = journal->records().front().target_head;
  stale_record.target_chain_hash      = journal->records().front().target_chain_hash;

  std::vector<MiniGitJournalRecord> records;
  records.push_back(stale_record);

  std::string error;
  auto        result = EditorMiniGitJournalFold::Fold(fold_graph, records, &error);
  EXPECT_FALSE(result.accepted);
  EXPECT_FALSE(result.error.empty());

  // Graph must be unchanged.
  EXPECT_EQ(fold_graph.GetActiveVersionRef().head_commit_hash, prior_head);
  EXPECT_EQ(fold_graph.CommitCount(), prior_commits);
}

/// Records that form a duplicate application (same expected source head on
/// consecutive records, or a chain hash that does not match the computed fold)
/// must cause the fold to reject without writing anything to the graph.
TEST_F(EditorMiniGitJournalFoldTest, MalformedDuplicateOrOutOfOrderRecordWritesNothing) {
  auto                  journal = std::make_shared<MiniGitJournal>();
  MiniGitWorkingHistory history(graph_, journal);
  ASSERT_TRUE(history.AppendEdit(MakeExposurePayload(0.0f, 1.0f)).committed);

  const auto& valid_record = journal->records().front();

  // Build two records that both try to apply from the SAME source head. The
  // first will succeed; the second must fail because the graph head has already
  // advanced.
  MiniGitJournalRecord dup;
  dup.kind                   = MiniGitJournalRecordKind::kEditCommit;
  dup.expected_source_head   = valid_record.expected_source_head;
  dup.expected_source_chain_hash = valid_record.expected_source_chain_hash;
  dup.edit_commit            = valid_record.edit_commit;
  dup.target_head            = valid_record.target_head;
  dup.target_chain_hash      = valid_record.target_chain_hash;

  auto fold_graph = *graph_;
  std::vector<MiniGitJournalRecord> records;
  records.push_back(dup);
  records.push_back(dup);  // Second copy with same source head → stale on second iteration.

  std::string error;
  auto        result = EditorMiniGitJournalFold::Fold(fold_graph, records, &error);
  EXPECT_FALSE(result.accepted);
  EXPECT_FALSE(result.error.empty());
}

}  // namespace alcedo
