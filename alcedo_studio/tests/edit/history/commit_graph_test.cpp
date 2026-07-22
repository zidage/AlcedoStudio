//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/history/commit_graph.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <json.hpp>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "app/project_package_backend.hpp"
#include "app/project_service.hpp"
#include "edit/history/commit_clock_test_access.hpp"
#include "edit/history/commit_types.hpp"
#include "edit/history/edit_commit.hpp"
#include "edit/history/version_ref.hpp"
#include "edit/operators/op_base.hpp"
#include "storage/controller/db_controller.hpp"
#include "storage/service/sleeve/edit_history/commit_graph_service.hpp"
#include "type/type.hpp"

namespace alcedo {
namespace {

auto MakeExposurePayload(float before, float after) -> OrdinaryEditPayload {
  OrdinaryEditPayload payload;
  payload.operator_type  = OperatorType::EXPOSURE;
  payload.stage_name     = PipelineStageName::Basic_Adjustment;
  payload.field_name     = "exposure";
  payload.before_value   = before;
  payload.after_value    = after;
  payload.before_enabled = true;
  payload.after_enabled  = true;
  return payload;
}

auto MakeContrastPayload(float before, float after) -> OrdinaryEditPayload {
  OrdinaryEditPayload payload;
  payload.operator_type  = OperatorType::CONTRAST;
  payload.stage_name     = PipelineStageName::Basic_Adjustment;
  payload.field_name     = "contrast";
  payload.before_value   = before;
  payload.after_value    = after;
  payload.before_enabled = true;
  payload.after_enabled  = true;
  return payload;
}

auto MakeEditAt(root_id_t root_id, head_commit_hash_t first_parent, std::uint64_t created_at_ns,
                OrdinaryEditPayload payload) -> EditCommit {
  return edit_history_test::EditCommitAccess::MakeEditAtTimestamp(
      root_id, std::move(first_parent), created_at_ns, std::move(payload));
}

auto MakeMergeAt(root_id_t root_id, head_commit_hash_t first_parent, commit_hash_t second_parent,
                 std::uint64_t created_at_ns, MergeEditPayload payload) -> EditCommit {
  return edit_history_test::EditCommitAccess::MakeMergeAtTimestamp(
      root_id, std::move(first_parent), second_parent, created_at_ns, std::move(payload));
}

}  // namespace

// 6C-1 empty-state boundary: infrastructure helper only. Production root creation after import
// metadata resolution is a later package and is not claimed here.
TEST(CommitGraphEmptyState, InfrastructureEmptyImageEditStateCreatesRootAndDefaultVersionRef) {
  auto graph = CommitGraph::CreateEmpty(42);
  EXPECT_EQ(graph.GetElementId(), 42u);
  EXPECT_NE(graph.GetRootId(), Hash128{});
  EXPECT_EQ(graph.GetAllVersionRefs().size(), 1u);

  const auto& active = graph.GetActiveVersionRef();
  EXPECT_EQ(active.element_id, 42u);
  EXPECT_EQ(active.display_name, "Default");
  EXPECT_FALSE(active.head_commit_hash.has_value());
  EXPECT_EQ(active.version_id, graph.GetActiveVersionId());
  EXPECT_EQ(graph.GetImageEditState().project_schema_version, kImageEditSchemaVersion);
  EXPECT_EQ(graph.GetImageEditState().materialized_transaction_chain_hash,
            ComputeRootChainHash(graph.GetRootId()));
  EXPECT_EQ(graph.CommitCount(), 0u);
}

TEST(CommitGraphEmptyState, VersionIdStaysUnchangedWhenWorkingHeadMoves) {
  edit_history_test::CommitClockAccess::ResetGlobal(0);
  auto       graph      = CommitGraph::CreateEmpty(7);

  const auto version_id = graph.GetActiveVersionId();
  auto       commit     = MakeEditAt(graph.GetRootId(), std::nullopt, CommitClock::NextGlobal(1000),
                                     MakeExposurePayload(0.0f, 1.0f));
  ASSERT_TRUE(graph.InsertCommit(commit));
  graph.MoveWorkingHead(version_id, commit.GetCommitHash(), 123);

  EXPECT_EQ(graph.GetActiveVersionId(), version_id);
  EXPECT_EQ(graph.GetActiveVersionRef().version_id, version_id);
  ASSERT_TRUE(graph.GetActiveVersionRef().head_commit_hash.has_value());
  EXPECT_EQ(*graph.GetActiveVersionRef().head_commit_hash, commit.GetCommitHash());
  // Working-head move must not claim DuckDB/materialized state advanced.
  EXPECT_FALSE(graph.GetImageEditState().materialized_head_commit_hash.has_value());
  EXPECT_EQ(graph.GetImageEditState().materialized_transaction_chain_hash,
            ComputeRootChainHash(graph.GetRootId()));
}

TEST(VersionRefCreation, ExplicitRootAndActiveHeadAreUnambiguousWhenActiveIsNonRoot) {
  edit_history_test::CommitClockAccess::ResetGlobal(0);
  auto graph  = CommitGraph::CreateEmpty(5);
  auto commit = MakeEditAt(graph.GetRootId(), std::nullopt, CommitClock::NextGlobal(10),
                           MakeExposurePayload(0.0f, 0.8f));
  ASSERT_TRUE(graph.InsertCommit(commit));
  graph.MoveWorkingHead(graph.GetActiveVersionId(), commit.GetCommitHash());

  const auto root_version = graph.CreateVersionRefAtRoot("RootBranch");
  EXPECT_FALSE(graph.GetVersionRef(root_version).head_commit_hash.has_value());
  EXPECT_NE(root_version, graph.GetActiveVersionId());

  const auto active_copy = graph.CreateVersionRefAtActiveHead("ActiveCopy");
  ASSERT_TRUE(graph.GetVersionRef(active_copy).head_commit_hash.has_value());
  EXPECT_EQ(*graph.GetVersionRef(active_copy).head_commit_hash, commit.GetCommitHash());

  const auto explicit_head = graph.CreateVersionRefAtHead("Explicit", commit.GetCommitHash());
  ASSERT_TRUE(graph.GetVersionRef(explicit_head).head_commit_hash.has_value());
  EXPECT_EQ(*graph.GetVersionRef(explicit_head).head_commit_hash, commit.GetCommitHash());
}

TEST(EditCommitHashing, EqualAdjustmentValuesAtDifferentTimestampsProduceDifferentHashes) {
  const root_id_t root{1, 2};
  const auto      payload = MakeExposurePayload(0.0f, 0.5f);

  auto            first   = MakeEditAt(root, std::nullopt, 100, payload);
  auto            second  = MakeEditAt(root, std::nullopt, 101, payload);

  EXPECT_NE(first.GetCommitHash(), second.GetCommitHash());
  EXPECT_EQ(first.GetPayloadJSON(), second.GetPayloadJSON());
}

TEST(EditCommitHashing, ProductionFactoriesAlwaysUseProcessWideClock) {
  edit_history_test::CommitClockAccess::ResetGlobal(0);
  const root_id_t root{5, 6};
  const auto      payload = MakeExposurePayload(0.0f, 0.5f);

  auto            first   = EditCommit::MakeEdit(root, std::nullopt, payload);
  auto            second  = EditCommit::MakeEdit(root, std::nullopt, payload);

  EXPECT_GT(second.GetCreatedAtNs(), first.GetCreatedAtNs());
  EXPECT_NE(second.GetCommitHash(), first.GetCommitHash());
}

TEST(EditCommitHashing, ParentOrderChangesMergeCommitHash) {
  edit_history_test::CommitClockAccess::ResetGlobal(0);
  const root_id_t root{9, 8};

  auto            branch_a =
      MakeEditAt(root, std::nullopt, CommitClock::NextGlobal(10), MakeExposurePayload(0.0f, 1.0f));
  auto branch_b =
      MakeEditAt(root, std::nullopt, CommitClock::NextGlobal(20), MakeContrastPayload(0.0f, 0.2f));

  MergeEditPayload merge_payload;
  MergeFieldDelta  field;
  field.operator_type    = OperatorType::EXPOSURE;
  field.stage_name       = PipelineStageName::Basic_Adjustment;
  field.field_name       = "exposure";
  field.resolved_value   = 0.7f;
  field.resolved_enabled = true;
  merge_payload.fields.push_back(field);

  auto merge_ab = MakeMergeAt(root, branch_a.GetCommitHash(), branch_b.GetCommitHash(),
                              CommitClock::NextGlobal(30), merge_payload);
  auto merge_ba = MakeMergeAt(root, branch_b.GetCommitHash(), branch_a.GetCommitHash(),
                              merge_ab.GetCreatedAtNs(), merge_payload);

  EXPECT_NE(merge_ab.GetCommitHash(), merge_ba.GetCommitHash());
}

TEST(EditCommitHashing, ReorderingEquivalentMergeFieldsDoesNotChangeHash) {
  const root_id_t root{3, 4};
  auto            a = MakeEditAt(root, std::nullopt, 1, MakeExposurePayload(0.0f, 0.1f));
  auto            b = MakeEditAt(root, std::nullopt, 2, MakeContrastPayload(0.0f, 0.2f));

  MergeFieldDelta exposure;
  exposure.operator_type    = OperatorType::EXPOSURE;
  exposure.stage_name       = PipelineStageName::Basic_Adjustment;
  exposure.field_name       = "exposure";
  exposure.resolved_value   = 0.3f;
  exposure.resolved_enabled = true;

  MergeFieldDelta contrast;
  contrast.operator_type    = OperatorType::CONTRAST;
  contrast.stage_name       = PipelineStageName::Basic_Adjustment;
  contrast.field_name       = "contrast";
  contrast.resolved_value   = 0.4f;
  contrast.resolved_enabled = true;

  MergeEditPayload ordered;
  ordered.fields = {exposure, contrast};
  MergeEditPayload reversed;
  reversed.fields = {contrast, exposure};

  auto m1         = MakeMergeAt(root, a.GetCommitHash(), b.GetCommitHash(), 50, ordered);
  auto m2         = MakeMergeAt(root, a.GetCommitHash(), b.GetCommitHash(), 50, reversed);
  EXPECT_EQ(m1.GetCommitHash(), m2.GetCommitHash());
  EXPECT_EQ(m1.GetPayloadJSON(), m2.GetPayloadJSON());

  // The payload serializer itself must emit a form accepted by its deserializer.
  const auto reversed_json = reversed.ToJSON();
  EXPECT_NO_THROW((void)MergeEditPayload::FromJSON(reversed_json));
  EXPECT_EQ(MergeEditPayload::FromJSON(reversed_json).ToJSON(), reversed_json);
}

TEST(EditCommitHashing, DuplicateMergeFieldIdentityIsRejected) {
  MergeEditPayload payload;
  MergeFieldDelta  field;
  field.operator_type  = OperatorType::EXPOSURE;
  field.stage_name     = PipelineStageName::Basic_Adjustment;
  field.field_name     = "exposure";
  field.resolved_value = 1.0f;
  payload.fields       = {field, field};
  EXPECT_THROW(payload.CanonicalizeAndValidate(), std::runtime_error);
}

TEST(EditCommitHashing, UnknownKindAndEditWithSecondParentAreRejected) {
  EXPECT_THROW(EditCommitKindFromString("rebase"), std::runtime_error);
  EXPECT_THROW(EditCommitKindFromInt(99), std::runtime_error);

  nlohmann::json j;
  j["commit_hash"]        = Hash128{1, 1}.ToString();
  j["root_id"]            = Hash128{2, 2}.ToString();
  j["first_parent_hash"]  = "";
  j["second_parent_hash"] = Hash128{3, 3}.ToString();
  j["created_at_ns"]      = 1;
  j["kind"]               = "edit";
  j["edit_payload"]       = MakeExposurePayload(0.0f, 1.0f).CanonicalJSON();
  EXPECT_THROW(EditCommit::FromJSON(j), std::runtime_error);
}

TEST(EditCommitHashing, NonCanonicalOrExtraPayloadFieldsAreRejected) {
  auto valid = MakeEditAt(Hash128{1, 2}, std::nullopt, 7, MakeExposurePayload(0.0f, 1.0f));
  auto j     = valid.ToJSON();

  // Extra field is corrupt structure.
  j["edit_payload"]["extra"] = true;
  EXPECT_THROW(EditCommit::FromJSON(j), std::runtime_error);

  // Missing required field_name is corrupt (no silent empty default).
  j = valid.ToJSON();
  j["edit_payload"].erase("field_name");
  EXPECT_THROW(EditCommit::FromJSON(j), std::runtime_error);

  // Invalid operator enum is corrupt.
  j                                  = valid.ToJSON();
  j["edit_payload"]["operator_type"] = 99999;
  EXPECT_THROW(EditCommit::FromJSON(j), std::runtime_error);

  // Non-canonical merge field order is corrupt even if values match.
  auto            a = MakeEditAt(Hash128{3, 4}, std::nullopt, 1, MakeExposurePayload(0.0f, 0.1f));
  auto            b = MakeEditAt(Hash128{3, 4}, std::nullopt, 2, MakeContrastPayload(0.0f, 0.2f));
  MergeFieldDelta exposure;
  exposure.operator_type    = OperatorType::EXPOSURE;
  exposure.stage_name       = PipelineStageName::Basic_Adjustment;
  exposure.field_name       = "exposure";
  exposure.resolved_value   = 0.3f;
  exposure.resolved_enabled = true;
  MergeFieldDelta contrast;
  contrast.operator_type    = OperatorType::CONTRAST;
  contrast.stage_name       = PipelineStageName::Basic_Adjustment;
  contrast.field_name       = "contrast";
  contrast.resolved_value   = 0.4f;
  contrast.resolved_enabled = true;
  MergeEditPayload ordered;
  ordered.fields = {exposure, contrast};
  auto merge     = MakeMergeAt(Hash128{3, 4}, a.GetCommitHash(), b.GetCommitHash(), 9, ordered);
  auto merge_j   = merge.ToJSON();
  // Reverse the stored field array so text is non-canonical.
  std::reverse(merge_j["edit_payload"]["fields"].begin(), merge_j["edit_payload"]["fields"].end());
  // Keep the hash matching the non-canonical text so only canonical-form validation fails.
  EXPECT_THROW(EditCommit::FromJSON(merge_j), std::runtime_error);
}

TEST(EditCommitHashing, NonReplayableOperatorAndStageSentinelsAreRejected) {
  auto payload          = MakeExposurePayload(0.0f, 1.0f);
  payload.operator_type = OperatorType::UNKNOWN;
  EXPECT_THROW(MakeEditAt(Hash128{1, 2}, std::nullopt, 1, payload), std::runtime_error);

  payload            = MakeExposurePayload(0.0f, 1.0f);
  payload.stage_name = PipelineStageName::Stage_Count;
  EXPECT_THROW(MakeEditAt(Hash128{1, 2}, std::nullopt, 2, payload), std::runtime_error);

  payload            = MakeExposurePayload(0.0f, 1.0f);
  payload.stage_name = PipelineStageName::Merged_Stage;
  EXPECT_THROW(MakeEditAt(Hash128{1, 2}, std::nullopt, 3, payload), std::runtime_error);
}

TEST(CommitGraphProjectionCapture, DefaultCapturePreservesProjectionAndClearIsExplicit) {
  edit_history_test::CommitClockAccess::ResetGlobal(0);
  auto graph  = CommitGraph::CreateEmpty(77);
  auto commit = MakeEditAt(graph.GetRootId(), std::nullopt, CommitClock::NextGlobal(1),
                           MakeExposurePayload(0.0f, 0.2f));
  ASSERT_TRUE(graph.InsertCommit(commit));
  graph.MoveWorkingHead(graph.GetActiveVersionId(), commit.GetCommitHash());

  const nlohmann::json projection = nlohmann::json{{"exposure", 0.2f}};
  auto                 with_proj  = graph.CaptureMaterializationWithProjection(projection);
  graph.ApplyMaterializedState(with_proj.image_state);
  ASSERT_TRUE(graph.GetImageEditState().stored_pipeline_projection.has_value());

  auto preserved = graph.CaptureMaterialization();
  ASSERT_TRUE(preserved.image_state.stored_pipeline_projection.has_value());
  EXPECT_EQ(*preserved.image_state.stored_pipeline_projection, projection);

  auto cleared = graph.CaptureMaterializationClearingProjection();
  EXPECT_FALSE(cleared.image_state.stored_pipeline_projection.has_value());
}

TEST(CommitGraphMaterializedState, FailedApplyLeavesPriorStateUnchanged) {
  auto graph                  = CommitGraph::CreateEmpty(78);
  auto before                 = graph.GetImageEditState();
  auto candidate              = before;
  candidate.active_version_id = Hash128{123, 456};

  EXPECT_THROW(graph.ApplyMaterializedState(candidate), std::runtime_error);
  EXPECT_EQ(graph.GetImageEditState().ToJSON(), before.ToJSON());
}

TEST(EditCommitHashing, CommitClockIsProcessWideStrictlyIncreasingAndThreadSafe) {
  edit_history_test::CommitClockAccess::ResetGlobal(0);
  EXPECT_EQ(CommitClock::NextGlobal(5), 5u);
  EXPECT_EQ(CommitClock::NextGlobal(5), 6u);
  EXPECT_EQ(CommitClock{}.Next(100), 100u);

  edit_history_test::CommitClockAccess::ResetGlobal(0);
  constexpr int              kThreads   = 8;
  constexpr int              kPerThread = 200;
  std::vector<std::thread>   threads;
  std::vector<std::uint64_t> stamps(static_cast<std::size_t>(kThreads * kPerThread));
  std::atomic<int>           index{0};
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&]() {
      for (int i = 0; i < kPerThread; ++i) {
        const auto stamp                       = CommitClock::NextGlobal(1);
        const int  slot                        = index.fetch_add(1);
        stamps[static_cast<std::size_t>(slot)] = stamp;
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  std::sort(stamps.begin(), stamps.end());
  for (std::size_t i = 1; i < stamps.size(); ++i) {
    EXPECT_LT(stamps[i - 1], stamps[i]);
  }

  edit_history_test::CommitClockAccess::ResetGlobal(std::numeric_limits<std::uint64_t>::max());
  EXPECT_THROW(CommitClock::NextGlobal(1), std::runtime_error);
  edit_history_test::CommitClockAccess::ResetGlobal(0);
}

TEST(EditCommitHashing, FixedHashVectorsAreStable) {
  // Fixed little-endian hash vectors for format version 1. Do not change without a schema bump.
  const root_id_t     root{0x1122334455667788ULL, 0x99aabbccddeeff00ULL};
  const auto          root_chain = ComputeRootChainHash(root);

  OrdinaryEditPayload payload;
  payload.operator_type   = OperatorType::EXPOSURE;
  payload.stage_name      = PipelineStageName::Basic_Adjustment;
  payload.field_name      = "exposure";
  payload.before_value    = 0.0;
  payload.after_value     = 1.25;
  payload.before_enabled  = true;
  payload.after_enabled   = true;

  auto       commit       = MakeEditAt(root, std::nullopt, 42, payload);
  const auto folded       = FoldTransactionChainHash(root_chain, commit.GetCommitHash());
  const auto root_input   = RootChainHashInput(root);
  const auto commit_input = commit.CanonicalHashInput();
  const auto fold_input   = TransactionChainFoldInput(root_chain, commit.GetCommitHash());

  // Recompute from the same canonical bytes.
  EXPECT_EQ(Hash128::Compute(root_input.data(), root_input.size()), root_chain);
  EXPECT_EQ(Hash128::Compute(commit_input.data(), commit_input.size()), commit.GetCommitHash());
  EXPECT_EQ(Hash128::Compute(fold_input.data(), fold_input.size()), folded);

  // Frozen golden hex strings for little-endian format version 1.
  EXPECT_EQ(root_chain.ToString(), "b086b9015c867f88aeca8730b1b8d55c");
  EXPECT_EQ(commit.GetCommitHash().ToString(), "02c397162017dc758e0c06ed5b9e0529");
  EXPECT_EQ(folded.ToString(), "e1b24bd4acb55030f625eb7dc7f47be2");
}

TEST(CommitGraphTraversal, FirstParentTraversalAndChainFoldAreDeterministic) {
  edit_history_test::CommitClockAccess::ResetGlobal(0);
  auto graph = CommitGraph::CreateEmpty(11);

  auto c1    = MakeEditAt(graph.GetRootId(), std::nullopt, CommitClock::NextGlobal(1),
                          MakeExposurePayload(0.0f, 0.1f));
  ASSERT_TRUE(graph.InsertCommit(c1));
  graph.MoveWorkingHead(graph.GetActiveVersionId(), c1.GetCommitHash());

  auto c2 = MakeEditAt(graph.GetRootId(), c1.GetCommitHash(), CommitClock::NextGlobal(2),
                       MakeContrastPayload(0.0f, 0.2f));
  ASSERT_TRUE(graph.InsertCommit(c2));
  graph.MoveWorkingHead(graph.GetActiveVersionId(), c2.GetCommitHash());

  const auto chain = graph.FirstParentChain(c2.GetCommitHash());
  ASSERT_EQ(chain.size(), 2u);
  EXPECT_EQ(chain[0], c1.GetCommitHash());
  EXPECT_EQ(chain[1], c2.GetCommitHash());
  EXPECT_EQ(graph.ChainHashForHead(c2.GetCommitHash()),
            FoldFirstParentChain(graph.GetRootId(), chain));
}

TEST(CommitGraphSharing, TwoVersionRefsShareOneCommitObjectWithoutDuplicatingRows) {
  edit_history_test::CommitClockAccess::ResetGlobal(0);
  auto graph  = CommitGraph::CreateEmpty(3);

  auto commit = MakeEditAt(graph.GetRootId(), std::nullopt, CommitClock::NextGlobal(10),
                           MakeExposurePayload(0.0f, 0.3f));
  ASSERT_TRUE(graph.InsertCommit(commit));
  graph.MoveWorkingHead(graph.GetActiveVersionId(), commit.GetCommitHash());

  const auto second_version =
      graph.CreateVersionRefAtHead("Alternate", commit.GetCommitHash(), /*created_at=*/50);
  EXPECT_NE(second_version, graph.GetActiveVersionId());
  EXPECT_EQ(graph.GetVersionRef(second_version).head_commit_hash, commit.GetCommitHash());
  EXPECT_EQ(graph.CommitCount(), 1u);
  EXPECT_FALSE(graph.InsertCommit(commit));
  EXPECT_EQ(graph.CommitCount(), 1u);
}

TEST(CommitGraphValidation, MissingOrCrossRootParentsFailImmediately) {
  edit_history_test::CommitClockAccess::ResetGlobal(0);
  auto graph         = CommitGraph::CreateEmpty(8);
  auto orphan_parent = MakeEditAt(Hash128{9, 9}, std::nullopt, 1, MakeExposurePayload(0.0f, 0.1f));
  auto child         = MakeEditAt(graph.GetRootId(), orphan_parent.GetCommitHash(), 2,
                                  MakeExposurePayload(0.1f, 0.2f));
  EXPECT_THROW(graph.InsertCommit(child), std::runtime_error);

  ImageEditState          state = graph.GetImageEditState();
  std::vector<VersionRef> refs;
  for (const auto& [id, ref] : graph.GetAllVersionRefs()) {
    (void)id;
    refs.push_back(ref);
  }
  // Missing first parent in FromParts.
  auto c1 = MakeEditAt(graph.GetRootId(), std::nullopt, 10, MakeExposurePayload(0, 1));
  auto c2 = MakeEditAt(graph.GetRootId(), c1.GetCommitHash(), 11, MakeExposurePayload(1, 2));
  state.materialized_head_commit_hash = c2.GetCommitHash();
  state.materialized_transaction_chain_hash =
      FoldFirstParentChain(graph.GetRootId(), {c1.GetCommitHash(), c2.GetCommitHash()});
  refs.front().head_commit_hash = c2.GetCommitHash();
  EXPECT_THROW(CommitGraph::FromParts(state, refs, {c2}), std::runtime_error);
}

class CommitGraphPersistenceTests : public ::testing::Test {
 protected:
  std::filesystem::path         db_path_;
  std::unique_ptr<DBController> db_;

  void                          SetUp() override {
    edit_history_test::CommitClockAccess::ResetGlobal(0);
    const auto stamp =
        std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    db_path_ = std::filesystem::temp_directory_path() / ("commit_graph_persist_" + stamp + ".db");
    std::error_code ec;
    std::filesystem::remove(db_path_, ec);
    db_ = std::make_unique<DBController>(db_path_);
  }

  void TearDown() override {
    db_.reset();
    std::error_code ec;
    std::filesystem::remove(db_path_, ec);
  }
};

TEST_F(CommitGraphPersistenceTests, TwoVersionRefsShareOneStoredCommitRow) {
  auto               guard = db_->GetConnectionGuard();
  auto               lock  = guard.Lock();
  CommitGraphService service(guard.conn_);

  auto               graph = CommitGraph::CreateEmpty(1001);
  auto commit              = MakeEditAt(graph.GetRootId(), std::nullopt, CommitClock::NextGlobal(1),
                                        MakeExposurePayload(0.0f, 1.25f));
  ASSERT_TRUE(graph.InsertCommit(commit));
  graph.MoveWorkingHead(graph.GetActiveVersionId(), commit.GetCommitHash());
  graph.CreateVersionRefAtHead("SharedHead", commit.GetCommitHash());

  const nlohmann::json projection      = nlohmann::json{{"exposure", 1.25f}};
  auto                 materialization = graph.CaptureMaterializationWithProjection(projection);
  ASSERT_NO_THROW(service.Materialize(materialization));
  graph.ApplyMaterializedState(materialization.image_state);
  EXPECT_EQ(service.CountCommitsForRoot(graph.GetRootId()), 1u);
  EXPECT_EQ(service.ListVersionRefsForElement(1001).size(), 2u);
  ASSERT_TRUE(graph.GetImageEditState().stored_pipeline_projection.has_value());
  EXPECT_EQ(*graph.GetImageEditState().stored_pipeline_projection, projection);

  // Default capture must preserve the existing projection, not clear it.
  ASSERT_NO_THROW(service.Materialize(graph.CaptureMaterialization()));
  EXPECT_EQ(service.CountCommitsForRoot(graph.GetRootId()), 1u);
  auto reloaded = service.LoadGraph(1001);
  ASSERT_TRUE(reloaded.has_value());
  ASSERT_TRUE(reloaded->GetImageEditState().stored_pipeline_projection.has_value());
  EXPECT_EQ(*reloaded->GetImageEditState().stored_pipeline_projection, projection);
}

TEST_F(CommitGraphPersistenceTests, InconsistentMaterializationLeavesPriorRowsUnchanged) {
  auto               guard = db_->GetConnectionGuard();
  auto               lock  = guard.Lock();
  CommitGraphService service(guard.conn_);

  auto               graph          = service.CreateEmptyPersisted(4004);
  const auto         baseline_count = service.CountCommits();
  auto               baseline_state = service.GetImageEditState(4004);
  ASSERT_TRUE(baseline_state.has_value());

  auto commit = MakeEditAt(graph.GetRootId(), std::nullopt, CommitClock::NextGlobal(1),
                           MakeExposurePayload(0.0f, 0.5f));
  ASSERT_TRUE(graph.InsertCommit(commit));
  graph.MoveWorkingHead(graph.GetActiveVersionId(), commit.GetCommitHash());

  auto bad                                            = graph.CaptureMaterialization();
  // Force disagreement: claim materialized head is root while Version points at commit.
  bad.image_state.materialized_head_commit_hash       = std::nullopt;
  bad.image_state.materialized_transaction_chain_hash = ComputeRootChainHash(graph.GetRootId());

  EXPECT_THROW(service.Materialize(bad), std::runtime_error);
  EXPECT_EQ(service.CountCommits(), baseline_count);
  auto after = service.GetImageEditState(4004);
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(after->materialized_head_commit_hash, baseline_state->materialized_head_commit_hash);
  EXPECT_EQ(after->materialized_transaction_chain_hash,
            baseline_state->materialized_transaction_chain_hash);
}

TEST_F(CommitGraphPersistenceTests, MaterializationSurvivesDbControllerRecreate) {
  root_id_t                  root_id{};
  version_ref_id_t           active_id{};
  commit_hash_t              head_hash{};
  transaction_chain_hash_t   chain_hash{};
  std::vector<commit_hash_t> first_parent_order;
  nlohmann::json             projection = nlohmann::json{{"contrast", 0.5f}};

  {
    auto               guard = db_->GetConnectionGuard();
    auto               lock  = guard.Lock();
    CommitGraphService service(guard.conn_);

    auto               graph = CommitGraph::CreateEmpty(2002);
    auto               c1 = MakeEditAt(graph.GetRootId(), std::nullopt, CommitClock::NextGlobal(1),
                                       MakeExposurePayload(0.0f, 0.4f));
    auto c2 = MakeEditAt(graph.GetRootId(), c1.GetCommitHash(), CommitClock::NextGlobal(2),
                         MakeContrastPayload(0.0f, 0.5f));
    ASSERT_TRUE(graph.InsertCommit(c1));
    ASSERT_TRUE(graph.InsertCommit(c2));
    graph.MoveWorkingHead(graph.GetActiveVersionId(), c2.GetCommitHash());

    root_id              = graph.GetRootId();
    active_id            = graph.GetActiveVersionId();
    head_hash            = c2.GetCommitHash();
    first_parent_order   = graph.FirstParentChain(c2.GetCommitHash());
    chain_hash           = graph.ChainHashForHead(c2.GetCommitHash());

    auto materialization = graph.CaptureMaterializationWithProjection(projection);
    service.Materialize(materialization);
  }

  // Close and recreate the controller against the same file.
  db_.reset();
  db_ = std::make_unique<DBController>(db_path_);

  {
    auto               guard = db_->GetConnectionGuard();
    auto               lock  = guard.Lock();
    CommitGraphService service(guard.conn_);

    auto               loaded = service.LoadGraph(2002);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->GetRootId(), root_id);
    EXPECT_EQ(loaded->GetActiveVersionId(), active_id);
    ASSERT_TRUE(loaded->GetActiveVersionRef().head_commit_hash.has_value());
    EXPECT_EQ(*loaded->GetActiveVersionRef().head_commit_hash, head_hash);
    EXPECT_EQ(loaded->GetImageEditState().materialized_head_commit_hash, head_hash);
    EXPECT_EQ(loaded->GetImageEditState().materialized_transaction_chain_hash, chain_hash);
    EXPECT_EQ(loaded->FirstParentChain(head_hash), first_parent_order);
    EXPECT_EQ(loaded->ChainHashForHead(head_hash), chain_hash);
    ASSERT_TRUE(loaded->GetImageEditState().stored_pipeline_projection.has_value());
    EXPECT_EQ(*loaded->GetImageEditState().stored_pipeline_projection, projection);
  }
}

TEST_F(CommitGraphPersistenceTests, EmptyPersistedImageHasRootAndDefaultVersionRef) {
  auto               guard = db_->GetConnectionGuard();
  auto               lock  = guard.Lock();
  CommitGraphService service(guard.conn_);

  CommitGraph        graph;
  ASSERT_NO_THROW(graph = service.CreateEmptyPersisted(3003));
  EXPECT_EQ(graph.CommitCount(), 0u);
  EXPECT_FALSE(graph.GetActiveVersionRef().head_commit_hash.has_value());

  auto loaded = service.LoadGraph(3003);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->GetRootId(), graph.GetRootId());
  EXPECT_EQ(loaded->GetActiveVersionId(), graph.GetActiveVersionId());
  EXPECT_EQ(loaded->GetAllVersionRefs().size(), 1u);
}

class ProjectSchemaBoundaryTests : public ::testing::Test {
 protected:
  std::filesystem::path db_path_;
  std::filesystem::path meta_path_;

  void                  SetUp() override {
    db_path_ = std::filesystem::temp_directory_path() / "project_schema_boundary.db";
    meta_path_ = std::filesystem::temp_directory_path() / "project_schema_boundary.json";
    std::error_code ec;
    std::filesystem::remove(db_path_, ec);
    std::filesystem::remove(meta_path_, ec);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove(db_path_, ec);
    std::filesystem::remove(meta_path_, ec);
  }
};

TEST_F(ProjectSchemaBoundaryTests, CurrentProjectFileVersionIsSupported) {
  EXPECT_TRUE(project_pack::ProjectVersionIsSupported(project_pack::kProjectFileVersion));
  EXPECT_EQ(project_pack::kProjectFileVersion, "0.3.0");
}

TEST_F(ProjectSchemaBoundaryTests, OldProjectFailsBeforeLoadingHistoryOrPipeline) {
  EXPECT_FALSE(project_pack::ProjectVersionIsSupported("0.2.5"));
  EXPECT_FALSE(project_pack::ProjectVersionIsSupported("0.2.4"));

  {
    ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
    project.SaveProject(meta_path_);
  }

  {
    std::ifstream in(meta_path_);
    ASSERT_TRUE(in.is_open());
    nlohmann::json metadata;
    in >> metadata;
    in.close();
    metadata["project_file_version"] = "0.2.5";
    std::ofstream out(meta_path_);
    ASSERT_TRUE(out.is_open());
    out << metadata.dump(4);
  }

  try {
    ProjectService project(db_path_, meta_path_, ProjectOpenMode::kLoadExisting);
    FAIL() << "Expected incompatible-format rejection for old project packages";
  } catch (const std::runtime_error& error) {
    const std::string message = error.what();
    EXPECT_NE(message.find("Incompatible project format"), std::string::npos);
    EXPECT_NE(message.find("0.2.5"), std::string::npos);
  }
}

TEST_F(ProjectSchemaBoundaryTests, NewProjectMetadataWritesCurrentSchemaVersion) {
  {
    ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
    project.SaveProject(meta_path_);
  }
  std::ifstream in(meta_path_);
  ASSERT_TRUE(in.is_open());
  nlohmann::json metadata;
  in >> metadata;
  EXPECT_EQ(metadata.at("project_file_version").get<std::string>(),
            std::string(project_pack::kProjectFileVersion));
}

}  // namespace alcedo
