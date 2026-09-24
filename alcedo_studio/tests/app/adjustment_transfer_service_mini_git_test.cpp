//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <string>

#include "app/adjustment_transfer_package_builder.hpp"
#include "app/adjustment_transfer_service.hpp"
#include "app/editor_pipeline_command_service.hpp"
#include "app/pipeline_service.hpp"
#include "edit/graph/adjustment_ownership.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/commit_types.hpp"
#include "edit/history/edit_commit.hpp"
#include "edit/history/pipeline_edit_batch.hpp"
#include "edit/history/version_ref.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/pipeline/pipeline_executor.hpp"
#include "support/document_transfer_test_support.hpp"
#include "support/editor_mini_git_project_fixture.hpp"

namespace alcedo {
namespace {

/// Capture a transferable Color Grade document with the requested exposure.
auto MakeExposurePackage(float exposure_value) -> AdjustmentTransferPackage {
  return test::MakeExposureTransferPackage(static_cast<double>(exposure_value));
}

/// Capture a transferable Color Grade document with the requested contrast.
auto MakeContrastPackage(float contrast_value) -> AdjustmentTransferPackage {
  auto document = CreateDefaultPipelineDocument();
  test::PatchDocumentField(&document, test::ColorGradeFieldTarget("contrast"),
                           nlohmann::json{{"contrast", contrast_value}});
  return CaptureDocumentTransfer(document);
}

/// Current-panel (Default Color Grade) exposure read from @p document, the only parameter store.
/// Paste remaps node ids, so the target is resolved from the document.
auto DocumentExposureEv(const PipelineDocument& document) -> double {
  std::string    error;
  const auto     target = CompleteCurrentPanelParameterTarget(document, "exposure", &error);
  nlohmann::json json;
  if (!target.has_value() || !ReadEditorParameterJson(document, *target, &json, &error)) {
    throw std::runtime_error(error);
  }
  return json.at("exposure_ev").get<double>();
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
      *graph, CreateDefaultPipelineDocument(), contrast_pkg, "Pasted Contrast");
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
/// Library Paste (AdjustmentTransferApplyCoordinator flow): the pasted Version document is built
/// from the root and bound by swapping the pointer.
TEST_F(AdjustmentTransferPasteMergeTest, PasteAsNewVersionBindsTargetDocumentWithoutMirror) {
  constexpr sl_element_id_t kElement = 303;
  auto                      guard    = pipeline_service_->LoadEditorPipeline(kElement);
  ASSERT_TRUE(guard && guard->pipeline_ && guard->commit_graph_ && guard->root_document_);
  auto&          graph = *guard->commit_graph_;

  const auto prior_document = guard->document_;
  const auto prior_exposure = DocumentExposureEv(*prior_document);
  const auto prior_version  = graph.GetActiveVersionId();
  const auto expected_state = graph.GetImageEditState();

  const auto pasted         = AdjustmentTransferService::PasteAsRootRelativeVersion(
      graph, *guard->root_document_, MakeExposurePackage(1.75f), "Pasted");
  ASSERT_TRUE(pasted.pasted) << pasted.error;
  std::string error;
  ASSERT_TRUE(pipeline_service_->RebuildActiveEditorPipeline(guard, &error)) << error;

  EXPECT_EQ(graph.GetActiveVersionId(), pasted.new_version_id);
  EXPECT_NE(graph.GetActiveVersionId(), prior_version);
  EXPECT_EQ(guard->working_head_commit_hash(), pasted.new_head);
  EXPECT_NE(guard->document_, prior_document);
  EXPECT_DOUBLE_EQ(DocumentExposureEv(*guard->document_), 1.75);
  EXPECT_DOUBLE_EQ(DocumentExposureEv(*prior_document), prior_exposure)
      << "the swapped-out document is not changed";
  {
    std::unique_lock<std::mutex> lock(guard->pipeline_->GetRenderLock());
    EXPECT_EQ(guard->pipeline_->GpuDagDocument(), guard->document_);
  }
  EXPECT_TRUE(guard->serialized_state_needs_writeback_);

  ASSERT_TRUE(pipeline_service_->PersistEditorHistoryState(guard, expected_state, &error)) << error;
  pipeline_service_->SavePipeline(guard);
}

TEST_F(AdjustmentTransferPasteMergeTest, PasteWithEmptyPackageReturnsError) {
  const auto                element_id = test::EditorMiniGitProjectFixture::kElementA;
  auto*                     graph      = project_.graph(element_id).get();

  AdjustmentTransferPackage empty_pkg;
  auto                      result = AdjustmentTransferService::PasteAsRootRelativeVersion(
      *graph, CreateDefaultPipelineDocument(), empty_pkg, "Empty Paste");
  EXPECT_FALSE(result.pasted);
  EXPECT_FALSE(result.error.empty());
}

TEST_F(AdjustmentTransferPasteMergeTest, TransferSurfaceHasNoPipelineMergeOperation) {
  const auto element_id = test::EditorMiniGitProjectFixture::kElementA;
  auto*      graph      = project_.graph(element_id).get();
  ASSERT_TRUE(project_.AppendExposureEdit(element_id, 0.0f, 1.0f));
  const auto package = MakeExposurePackage(2.0f);
  const auto pasted  = AdjustmentTransferService::PasteAsRootRelativeVersion(
      *graph, CreateDefaultPipelineDocument(), package, "Pasted");
  ASSERT_TRUE(pasted.pasted) << pasted.error;
  const auto& commit = graph->GetCommit(pasted.new_head);
  ASSERT_TRUE(IsPipelineEditBatchJson(commit.GetPayloadJSON()));
  EXPECT_EQ(PipelineEditBatch::FromJSON(commit.GetPayloadJSON()).operation_kind,
            PipelineEditOperationKind::Paste);
}

// ============================================================================
// Robustness / edge case tests
// ============================================================================

/// One Paste action publishes one typed batch commit whose first parent is root.
TEST_F(AdjustmentTransferPasteMergeTest, CompleteGradeChainPasteCreatesOneTypedCommit) {
  const auto element_id   = test::EditorMiniGitProjectFixture::kElementA;
  auto*      graph        = project_.graph(element_id).get();

  auto       pkg          = MakeContrastPackage(0.3f);
  auto       paste_result = AdjustmentTransferService::PasteAsRootRelativeVersion(
      *graph, CreateDefaultPipelineDocument(), pkg, "Multi Paste");
  ASSERT_TRUE(paste_result.pasted) << paste_result.error;

  auto chain = graph->FirstParentChain(paste_result.new_head);
  EXPECT_EQ(chain.size(), 1u);
  const auto& commit = graph->GetCommit(chain[0]);
  EXPECT_EQ(commit.GetFirstParentHash(), std::nullopt);
  ASSERT_TRUE(IsPipelineEditBatchJson(commit.GetPayloadJSON()));
  EXPECT_EQ(PipelineEditBatch::FromJSON(commit.GetPayloadJSON()).operation_kind,
            PipelineEditOperationKind::Paste);
}

/// Paste creates a distinct Version even when the exact same commit objects already
/// exist from a prior paste.
TEST_F(AdjustmentTransferPasteMergeTest, RepeatedPasteCreatesDistinctVersionRef) {
  const auto element_id = test::EditorMiniGitProjectFixture::kElementA;
  auto*      graph      = project_.graph(element_id).get();

  auto       pkg        = MakeExposurePackage(1.0f);

  auto       first      = AdjustmentTransferService::PasteAsRootRelativeVersion(
      *graph, CreateDefaultPipelineDocument(), pkg, "First Paste");
  ASSERT_TRUE(first.pasted);
  const auto first_version_id = first.new_version_id;

  // Second paste with the same package.
  auto       second           = AdjustmentTransferService::PasteAsRootRelativeVersion(
      *graph, CreateDefaultPipelineDocument(), pkg, "Second Paste");
  ASSERT_TRUE(second.pasted);

  // Different Version ID, same commit objects (shared).
  EXPECT_NE(second.new_version_id, first_version_id);
  // Different Version ID. Commit count may have increased because EditCommit hashes
  // include the creation timestamp, so two otherwise-identical packages produce
  // different commit hashes.
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
      *graph, CreateDefaultPipelineDocument(), pkg, "Pasted");
  ASSERT_TRUE(paste_result.pasted);

  // The second Version should still exist and be at root.
  const auto& second_ref = graph->GetVersionRef(second_version_id);
  EXPECT_EQ(second_ref.head_commit_hash, std::nullopt);
  EXPECT_EQ(second_ref.version_id, second_version_id);
}

/// One selective Paste creates exactly one root-relative Version with exactly one
/// typed Paste commit whose first parent is the image root.
TEST_F(AdjustmentTransferPasteMergeTest, SelectivePasteCreatesOneRootRelativeVersionAndCommit) {
  const auto element_id = test::EditorMiniGitProjectFixture::kElementA;
  auto*      graph      = project_.graph(element_id).get();
  ASSERT_NE(graph, nullptr);

  const auto version_count_before = graph->GetAllVersionRefs().size();
  const auto commit_count_before  = graph->CommitCount();
  const auto prior_version_id     = graph->GetActiveVersionId();

  // A sparse package that selects only the exposure adjustment.
  auto       document     = test::DocumentWithExposureEv(1.5);
  const auto* grade       = document.PrimaryGrade();
  ASSERT_NE(grade, nullptr);
  const auto* exposure_id = grade->FindAdjustmentIdByType(type_ids::Exposure());
  ASSERT_NE(exposure_id, nullptr);
  AdjustmentTransferSelection selection;
  selection.nodes.push_back(
      {grade->Id(), {{AdjustmentTransferItemKind::Adjustment, *exposure_id}}});
  const auto package = AdjustmentTransferPackageBuilder::Build(document, selection);

  const auto result = AdjustmentTransferService::PasteAsRootRelativeVersion(
      *graph, CreateDefaultPipelineDocument(), package, "Selective Paste");
  ASSERT_TRUE(result.pasted) << result.error;

  EXPECT_EQ(graph->GetAllVersionRefs().size(), version_count_before + 1u);
  EXPECT_EQ(graph->CommitCount(), commit_count_before + 1u);
  EXPECT_EQ(graph->GetActiveVersionId(), result.new_version_id);
  EXPECT_NE(result.new_version_id, prior_version_id);

  const auto chain = graph->FirstParentChain(result.new_head);
  ASSERT_EQ(chain.size(), 1u);
  const auto& commit = graph->GetCommit(chain.front());
  EXPECT_EQ(commit.GetFirstParentHash(), std::nullopt);
  ASSERT_TRUE(IsPipelineEditBatchJson(commit.GetPayloadJSON()));
  EXPECT_EQ(PipelineEditBatch::FromJSON(commit.GetPayloadJSON()).operation_kind,
            PipelineEditOperationKind::Paste);
}

/// A planner failure creates no Version ref, no commit, and no active-Version change.
TEST_F(AdjustmentTransferPasteMergeTest, SelectivePasteFailureCreatesNoVersionOrCommit) {
  const auto element_id = test::EditorMiniGitProjectFixture::kElementA;
  auto*      graph      = project_.graph(element_id).get();
  ASSERT_NE(graph, nullptr);
  ASSERT_TRUE(project_.AppendExposureEdit(element_id, 0.0f, 1.0f));

  const auto version_count_before = graph->GetAllVersionRefs().size();
  const auto commit_count_before  = graph->CommitCount();
  const auto prior_version_id     = graph->GetActiveVersionId();
  const auto prior_head           = graph->GetActiveVersionRef().head_commit_hash;

  class CollidingIdentity final : public TransferIdentitySource {
   public:
    auto NextNodeId() -> NodeId override { return NodeId{"grade.primary"}; }
    auto NextAdjustmentInstanceId(const NodeId& node_id, const OperatorTypeId& type)
        -> AdjustmentInstanceId override {
      return MakeAdjustmentInstanceId(node_id, type);
    }
    auto NextMaskId() -> MaskId override { return MaskId{"mask.t1"}; }
#ifdef ALCEDO_ENABLE_BRUSH_MASK
    auto NextStrokeId() -> StrokeId override { return StrokeId{"stroke.t1"}; }
#endif
  } colliding;
  DocumentTransferPasteOptions options;
  options.identity_source = &colliding;

  const auto result = AdjustmentTransferService::PasteAsRootRelativeVersion(
      *graph, CreateDefaultPipelineDocument(), test::MakeExposureTransferPackage(2.0),
      "Rejected Paste", options);
  EXPECT_FALSE(result.pasted);
  EXPECT_FALSE(result.error.empty());

  EXPECT_EQ(graph->GetAllVersionRefs().size(), version_count_before);
  EXPECT_EQ(graph->CommitCount(), commit_count_before);
  EXPECT_EQ(graph->GetActiveVersionId(), prior_version_id);
  EXPECT_EQ(graph->GetActiveVersionRef().head_commit_hash, prior_head);
}

}  // namespace
}  // namespace alcedo
