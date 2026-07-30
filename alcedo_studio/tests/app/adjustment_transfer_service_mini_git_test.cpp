//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

#include "app/adjustment_transfer_service.hpp"
#include "app/pipeline_service.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/commit_types.hpp"
#include "edit/history/edit_commit.hpp"
#include "edit/history/version_ref.hpp"
#include "edit/operators/op_base.hpp"
#include "support/editor_mini_git_project_fixture.hpp"

namespace alcedo {
namespace {

/// Build a simple transfer package with one exposure adjustment.
auto MakeExposurePackage(float exposure_value, bool enabled = true) -> AdjustmentTransferPackage {
  AdjustmentTransferPackage package;
  package.operators_.push_back({
      .stage_         = PipelineStageName::Basic_Adjustment,
      .operator_type_ = OperatorType::EXPOSURE,
      .enabled_       = enabled,
      .merge_params_  = false,
      .params_        = {{"exposure", exposure_value}},
  });
  return package;
}

/// Build a transfer package with one contrast adjustment.
auto MakeContrastPackage(float contrast_value) -> AdjustmentTransferPackage {
  AdjustmentTransferPackage package;
  package.operators_.push_back({
      .stage_         = PipelineStageName::Basic_Adjustment,
      .operator_type_ = OperatorType::CONTRAST,
      .enabled_       = true,
      .merge_params_  = false,
      .params_        = {{"contrast", contrast_value}},
  });
  return package;
}

/// Build a transfer package with exposure and contrast.
auto MakeExposureContrastPackage(float exposure_value, float contrast_value)
    -> AdjustmentTransferPackage {
  AdjustmentTransferPackage package;
  package.operators_.push_back({
      .stage_         = PipelineStageName::Basic_Adjustment,
      .operator_type_ = OperatorType::EXPOSURE,
      .enabled_       = true,
      .merge_params_  = false,
      .params_        = {{"exposure", exposure_value}},
  });
  package.operators_.push_back({
      .stage_         = PipelineStageName::Basic_Adjustment,
      .operator_type_ = OperatorType::CONTRAST,
      .enabled_       = true,
      .merge_params_  = false,
      .params_        = {{"contrast", contrast_value}},
  });
  return package;
}

// ============================================================================
// Paste
// ============================================================================

class AdjustmentTransferPasteMergeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    project_.SetUp();
    pipeline_service_ = std::make_unique<PipelineMgmtService>(project_.storage());
  }
  void TearDown() override {
    pipeline_service_.reset();
    project_.TearDown();
  }

  test::EditorMiniGitProjectFixture    project_;
  std::unique_ptr<PipelineMgmtService> pipeline_service_;
};

/// Paste creates a new root-relative Version that does not inherit the previously
/// active Version's commits. The old Version keeps its head and ancestry; the new
/// Version has only the pasted commits rooted at the image root.
TEST_F(AdjustmentTransferPasteMergeTest,
       PasteCreatesRootRelativeVersionWithoutInheritingPriorCommits) {
  const auto element_id = test::EditorMiniGitProjectFixture::kElementA;
  auto*      graph      = project_.graph(element_id).get();
  ASSERT_NE(graph, nullptr);

  const auto original_version_id = graph->GetActiveVersionId();
  const auto original_head       = graph->GetActiveVersionRef().head_commit_hash;
  // Before any edits, head is null (root).
  EXPECT_EQ(original_head, std::nullopt);
  EXPECT_EQ(graph->CommitCount(), 0u);

  // Append an exposure edit via journal to create one commit on the default Version.
  ASSERT_TRUE(project_.AppendExposureEdit(element_id, 0.0f, 1.0f));
  EXPECT_EQ(graph->CommitCount(), 1u);
  const auto edited_head = graph->GetActiveVersionRef().head_commit_hash;
  ASSERT_TRUE(edited_head.has_value());
  // The active Version ID does not change when its head moves.
  EXPECT_EQ(graph->GetActiveVersionId(), original_version_id);

  // Paste a contrast adjustment as a new Version.
  auto contrast_pkg = MakeContrastPackage(0.5f);
  auto paste_result = AdjustmentTransferService::PasteAsRootRelativeVersion(
      *graph, *pipeline_service_, element_id, contrast_pkg, "Pasted Contrast");
  ASSERT_TRUE(paste_result.pasted) << paste_result.error;

  // The new Version is now active.
  const auto pasted_version_id = graph->GetActiveVersionId();
  EXPECT_NE(pasted_version_id, original_version_id);

  // The pasted Version's commits are root-relative — only the contrast commit.
  const auto pasted_head = graph->GetActiveVersionRef().head_commit_hash;
  ASSERT_TRUE(pasted_head.has_value());
  EXPECT_EQ(pasted_head, paste_result.new_head);

  // Walk first parents from the pasted head: should be exactly one commit (contrast)
  // whose parent is null (root). The exposure commit is not in this chain.
  auto pasted_chain = graph->FirstParentChain(pasted_head);
  EXPECT_EQ(pasted_chain.size(), 1u);

  // Check that the commit's first parent is NULL (root).
  const auto& pasted_commit = graph->GetCommit(pasted_chain[0]);
  EXPECT_EQ(pasted_commit.GetFirstParentHash(), std::nullopt);

  // The old Version's head is still reachable — it swapped to root, added exposure commit,
  // but that chain is on the original Version, not the pasted one.
  // Since we switched active Version, the original Version ref should still have its head.
  const auto& original_ref = graph->GetVersionRef(original_version_id);
  EXPECT_EQ(original_ref.head_commit_hash, edited_head);
}

/// Paste with an empty package returns an error.
TEST_F(AdjustmentTransferPasteMergeTest, PasteWithEmptyPackageReturnsError) {
  const auto                element_id = test::EditorMiniGitProjectFixture::kElementA;
  auto*                     graph      = project_.graph(element_id).get();

  AdjustmentTransferPackage empty_pkg;
  auto                      result = AdjustmentTransferService::PasteAsRootRelativeVersion(
      *graph, *pipeline_service_, element_id, empty_pkg, "Empty Paste");
  EXPECT_FALSE(result.pasted);
  EXPECT_FALSE(result.error.empty());
}

// ============================================================================
// Merge
// ============================================================================

/// InitiateMerge detects conflicts when the incoming package differs from the
/// current pipeline state.
TEST_F(AdjustmentTransferPasteMergeTest, InitiateMergeDetectsConflictsWhenValuesDiffer) {
  const auto element_id = test::EditorMiniGitProjectFixture::kElementA;
  auto*      graph      = project_.graph(element_id).get();

  // Append an exposure edit: before=0.0, after=1.0.
  ASSERT_TRUE(project_.AppendExposureEdit(element_id, 0.0f, 1.0f));
  EXPECT_EQ(graph->CommitCount(), 1u);

  // Initiate a merge with a different exposure value.
  auto pkg     = MakeExposurePackage(2.0f);
  auto preview = AdjustmentTransferService::InitiateMerge(*graph, *pipeline_service_, element_id,
                                                          pkg, "Merge Exposure");

  ASSERT_TRUE(preview.error.empty()) << preview.error;
  // The current pipeline has exposure=1.0, incoming has exposure=2.0 — conflict expected.
  EXPECT_TRUE(preview.has_conflicts);
  EXPECT_EQ(preview.conflicts.size(), 1u);
  EXPECT_EQ(preview.conflicts[0].operator_type, OperatorType::EXPOSURE);
}

/// InitiateMerge detects no conflicts when the incoming package matches the
/// current pipeline state.
TEST_F(AdjustmentTransferPasteMergeTest, InitiateMergeDetectsNoConflictsWhenValuesMatch) {
  const auto element_id = test::EditorMiniGitProjectFixture::kElementA;
  auto*      graph      = project_.graph(element_id).get();

  // Append an exposure edit: before=0.0, after=1.5.
  ASSERT_TRUE(project_.AppendExposureEdit(element_id, 0.0f, 1.5f));

  // Initiate a merge with the same exposure value.
  auto pkg     = MakeExposurePackage(1.5f);
  auto preview = AdjustmentTransferService::InitiateMerge(*graph, *pipeline_service_, element_id,
                                                          pkg, "Merge Exposure");

  ASSERT_TRUE(preview.error.empty()) << preview.error;
  EXPECT_FALSE(preview.has_conflicts);
  EXPECT_TRUE(preview.conflicts.empty());
}

/// CompleteMerge fails when there are unresolved conflicts.
TEST_F(AdjustmentTransferPasteMergeTest, CompleteMergeFailsWithUnresolvedConflicts) {
  const auto element_id = test::EditorMiniGitProjectFixture::kElementA;
  auto*      graph      = project_.graph(element_id).get();

  ASSERT_TRUE(project_.AppendExposureEdit(element_id, 0.0f, 1.0f));
  const auto head_before = graph->GetActiveVersionRef().head_commit_hash;

  auto       pkg         = MakeExposurePackage(2.0f);
  auto preview = AdjustmentTransferService::InitiateMerge(*graph, *pipeline_service_, element_id,
                                                          pkg, "Merge Exposure");
  ASSERT_TRUE(preview.has_conflicts);

  // CompleteMerge with empty resolutions should fail.
  std::vector<AdjustmentMergeResolution> empty_resolutions;
  auto result = AdjustmentTransferService::CompleteMerge(*graph, *pipeline_service_, preview,
                                                         empty_resolutions);
  EXPECT_FALSE(result.merged);
  EXPECT_FALSE(result.error.empty());

  // The active Version head must NOT have moved.
  EXPECT_EQ(graph->GetActiveVersionRef().head_commit_hash, head_before);
  // Commit count: the original edit (1) + the incoming branch commit(s) inserted by InitiateMerge.
  // CompleteMerge failing should not add a merge commit on top.
  EXPECT_EQ(graph->CommitCount(), 2u);
}

/// CompleteMerge creates a two-parent merge commit after all conflicts are resolved.
TEST_F(AdjustmentTransferPasteMergeTest, CompleteMergeCreatesTwoParentCommitWithResolvedValues) {
  const auto element_id = test::EditorMiniGitProjectFixture::kElementA;
  auto*      graph      = project_.graph(element_id).get();

  ASSERT_TRUE(project_.AppendExposureEdit(element_id, 0.0f, 1.0f));
  const auto current_head = graph->GetActiveVersionRef().head_commit_hash;
  ASSERT_TRUE(current_head.has_value());
  const auto version_id_before = graph->GetActiveVersionId();

  auto       pkg               = MakeExposurePackage(2.0f);
  auto preview = AdjustmentTransferService::InitiateMerge(*graph, *pipeline_service_, element_id,
                                                          pkg, "Merge Exposure");
  ASSERT_TRUE(preview.has_conflicts);
  ASSERT_EQ(preview.conflicts.size(), 1u);

  // Resolve with incoming value.
  std::vector<AdjustmentMergeResolution> resolutions;
  resolutions.push_back({
      .field_key        = preview.conflicts[0].field_key,
      .resolved_value   = preview.conflicts[0].incoming_value,
      .resolved_enabled = true,
  });

  auto result =
      AdjustmentTransferService::CompleteMerge(*graph, *pipeline_service_, preview, resolutions);
  ASSERT_TRUE(result.merged) << result.error;

  // Verify the merge commit.
  const auto  merge_hash   = result.merge_commit_hash;
  const auto& merge_commit = graph->GetCommit(merge_hash);
  EXPECT_EQ(merge_commit.GetKind(), EditCommitKind::kMerge);
  EXPECT_EQ(merge_commit.GetFirstParentHash(), current_head);
  EXPECT_EQ(merge_commit.GetSecondParentHash(), preview.incoming_head);

  // The active Version advanced to the merge commit.
  EXPECT_EQ(graph->GetActiveVersionId(), version_id_before);  // same Version ID
  EXPECT_EQ(graph->GetActiveVersionRef().head_commit_hash, merge_hash);
  EXPECT_THROW(graph->GetVersionRef(preview.incoming_version_id), std::runtime_error);

  // The merge commit is in the first-parent chain.
  auto chain = graph->FirstParentChain(merge_hash);
  EXPECT_GE(chain.size(), 2u);  // root -> edit -> merge or root -> merge -> ...

  // Both parents should be reachable in the graph.
  EXPECT_NE(graph->FindCommit(*merge_commit.GetFirstParentHash()), nullptr);
  EXPECT_NE(graph->FindCommit(*merge_commit.GetSecondParentHash()), nullptr);
}

/// CancelMerge does not create a commit or move the active Version ref.
TEST_F(AdjustmentTransferPasteMergeTest, CancelMergeLeavesNoCommitOrRefChange) {
  const auto element_id = test::EditorMiniGitProjectFixture::kElementA;
  auto*      graph      = project_.graph(element_id).get();

  ASSERT_TRUE(project_.AppendExposureEdit(element_id, 0.0f, 1.0f));
  const auto head_before         = graph->GetActiveVersionRef().head_commit_hash;
  const auto version_id_before   = graph->GetActiveVersionId();
  const auto commit_count_before = graph->CommitCount();

  auto       pkg                 = MakeContrastPackage(0.5f);
  auto preview = AdjustmentTransferService::InitiateMerge(*graph, *pipeline_service_, element_id,
                                                          pkg, "Merge Contrast");
  ASSERT_TRUE(preview.error.empty()) << preview.error;

  AdjustmentTransferService::CancelMerge(*graph, preview);

  // No change to the active Version or head.
  EXPECT_EQ(graph->GetActiveVersionId(), version_id_before);
  EXPECT_EQ(graph->GetActiveVersionRef().head_commit_hash, head_before);

  // The incoming branch commits are in the graph (they were inserted by InitiateMerge)
  // but the active Version does not reference them.
  // Commit count should have increased (incoming commits were inserted) but the
  // active Version doesn't move.
  EXPECT_GT(graph->CommitCount(), commit_count_before);
}

// ============================================================================
// Robustness / edge case tests
// ============================================================================

/// Multi-operator paste creates a commit chain with correct parent linkage.
TEST_F(AdjustmentTransferPasteMergeTest, MultiOperatorPasteCreatesCorrectlyLinkedCommitChain) {
  const auto element_id   = test::EditorMiniGitProjectFixture::kElementA;
  auto*      graph        = project_.graph(element_id).get();

  auto       pkg          = MakeExposureContrastPackage(1.5f, 0.3f);
  auto       paste_result = AdjustmentTransferService::PasteAsRootRelativeVersion(
      *graph, *pipeline_service_, element_id, pkg, "Multi Paste");
  ASSERT_TRUE(paste_result.pasted) << paste_result.error;

  // The commit chain should have two commits linked by first parent.
  auto chain = graph->FirstParentChain(paste_result.new_head);
  EXPECT_EQ(chain.size(), 2u);

  // First commit's parent is root; second commit's parent is the first commit.
  const auto& first  = graph->GetCommit(chain[0]);
  const auto& second = graph->GetCommit(chain[1]);
  EXPECT_EQ(first.GetFirstParentHash(), std::nullopt);
  EXPECT_EQ(second.GetFirstParentHash(), first.GetCommitHash());
}

/// Paste creates a distinct Version even when the exact same commit objects already
/// exist from a prior paste.
TEST_F(AdjustmentTransferPasteMergeTest, RepeatedPasteCreatesDistinctVersionRef) {
  const auto element_id = test::EditorMiniGitProjectFixture::kElementA;
  auto*      graph      = project_.graph(element_id).get();

  auto       pkg        = MakeExposurePackage(1.0f);

  auto       first      = AdjustmentTransferService::PasteAsRootRelativeVersion(
      *graph, *pipeline_service_, element_id, pkg, "First Paste");
  ASSERT_TRUE(first.pasted);
  const auto first_version_id = first.new_version_id;

  // Second paste with the same package.
  auto       second           = AdjustmentTransferService::PasteAsRootRelativeVersion(
      *graph, *pipeline_service_, element_id, pkg, "Second Paste");
  ASSERT_TRUE(second.pasted);

  // Different Version ID, same commit objects (shared).
  EXPECT_NE(second.new_version_id, first_version_id);
  // Different Version ID. Commit count may have increased because EditCommit hashes
  // include the creation timestamp, so two otherwise-identical packages produce
  // different commit hashes.
}

/// CompleteMerge with an errored preview rejects the call immediately.
TEST_F(AdjustmentTransferPasteMergeTest, CompleteMergeWithErroredPreviewRejectsImmediately) {
  const auto             element_id = test::EditorMiniGitProjectFixture::kElementA;
  auto*                  graph      = project_.graph(element_id).get();

  AdjustmentMergePreview errored_preview;
  errored_preview.error = "Simulated initiation failure";

  std::vector<AdjustmentMergeResolution> resolutions;
  auto result = AdjustmentTransferService::CompleteMerge(*graph, *pipeline_service_,
                                                         errored_preview, resolutions);
  EXPECT_FALSE(result.merged);
  EXPECT_FALSE(result.error.empty());
}

/// InitiateMerge with an empty package returns an error without modifying the graph.
TEST_F(AdjustmentTransferPasteMergeTest, InitiateMergeWithEmptyPackageReturnsError) {
  const auto                element_id          = test::EditorMiniGitProjectFixture::kElementA;
  auto*                     graph               = project_.graph(element_id).get();
  const auto                commit_count_before = graph->CommitCount();

  AdjustmentTransferPackage empty_pkg;
  auto preview = AdjustmentTransferService::InitiateMerge(*graph, *pipeline_service_, element_id,
                                                          empty_pkg, "Empty Merge");
  EXPECT_FALSE(preview.error.empty());
  EXPECT_EQ(graph->CommitCount(), commit_count_before);  // No commits inserted
}

/// Paste does not affect other Versions in the graph.
TEST_F(AdjustmentTransferPasteMergeTest, PasteDoesNotAffectOtherVersions) {
  const auto element_id          = test::EditorMiniGitProjectFixture::kElementA;
  auto*      graph               = project_.graph(element_id).get();

  const auto original_version_id = graph->GetActiveVersionId();

  // Create a second Version manually (at root).
  auto       second_version_id   = graph->CreateVersionRefAtHead("Second", std::nullopt);

  // Paste onto the active Version.
  graph->SetActiveVersionId(original_version_id);
  auto pkg          = MakeExposurePackage(2.0f);
  auto paste_result = AdjustmentTransferService::PasteAsRootRelativeVersion(
      *graph, *pipeline_service_, element_id, pkg, "Pasted");
  ASSERT_TRUE(paste_result.pasted);

  // The second Version should still exist and be at root.
  const auto& second_ref = graph->GetVersionRef(second_version_id);
  EXPECT_EQ(second_ref.head_commit_hash, std::nullopt);
  EXPECT_EQ(second_ref.version_id, second_version_id);
}

/// Merge commit appears in first-parent chain after creation.
TEST_F(AdjustmentTransferPasteMergeTest, MergeCommitAppearsInFirstParentChain) {
  const auto element_id = test::EditorMiniGitProjectFixture::kElementA;
  auto*      graph      = project_.graph(element_id).get();

  ASSERT_TRUE(project_.AppendExposureEdit(element_id, 0.0f, 1.0f));

  auto pkg     = MakeExposurePackage(2.0f);
  auto preview = AdjustmentTransferService::InitiateMerge(*graph, *pipeline_service_, element_id,
                                                          pkg, "Merge Chain Test");
  ASSERT_TRUE(preview.has_conflicts);

  std::vector<AdjustmentMergeResolution> resolutions;
  resolutions.push_back({
      .field_key        = preview.conflicts[0].field_key,
      .resolved_value   = preview.conflicts[0].incoming_value,
      .resolved_enabled = true,
  });
  auto result =
      AdjustmentTransferService::CompleteMerge(*graph, *pipeline_service_, preview, resolutions);
  ASSERT_TRUE(result.merged);

  // The merge commit must be in the first-parent chain.
  auto chain       = graph->FirstParentChain(graph->GetActiveVersionRef().head_commit_hash);
  bool found_merge = false;
  for (const auto& hash : chain) {
    if (hash == result.merge_commit_hash) {
      found_merge = true;
      break;
    }
  }
  EXPECT_TRUE(found_merge);
}
}  // namespace
}  // namespace alcedo
