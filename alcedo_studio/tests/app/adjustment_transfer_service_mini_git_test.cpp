//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <algorithm>
#include <mutex>
#include <string>

#include "app/adjustment_transfer_service.hpp"
#include "app/pipeline_service.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/commit_types.hpp"
#include "edit/history/edit_commit.hpp"
#include "edit/history/pipeline_edit_batch.hpp"
#include "edit/history/version_ref.hpp"
#include "edit/operators/basic/color_temp_op.hpp"
#include "edit/operators/geometry/lens_calib_op.hpp"
#include "edit/operators/op_base.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"
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

// ============================================================================
// Operator-owned merge policy (color_temp / lens_calib)
// ============================================================================

TEST(ColorTempOpMergePolicyTest, BothAsShotDoesNotConflictEvenWhenCctDiffers) {
  ColorTempOp op;
  const nlohmann::json current = {
      {"color_temp",
       {{"mode", "as_shot"},
        {"custom_cct", 5200.0},
        {"custom_tint", -3.0},
        {"as_shot_cct", 5200.0},
        {"as_shot_tint", -3.0}}}};
  const nlohmann::json incoming = {{"color_temp", {{"mode", "as_shot"}}}};
  EXPECT_FALSE(op.DetectMergeConflict(current, incoming));
}

TEST(ColorTempOpMergePolicyTest, CustomVersusAsShotConflicts) {
  ColorTempOp op;
  const nlohmann::json current = {
      {"color_temp",
       {{"mode", "custom"},
        {"custom_cct", 7000.0},
        {"custom_tint", 10.0},
        {"as_shot_cct", 5200.0},
        {"as_shot_tint", -3.0}}}};
  const nlohmann::json incoming = {{"color_temp", {{"mode", "as_shot"}}}};
  EXPECT_TRUE(op.DetectMergeConflict(current, incoming));
}

TEST(ColorTempOpMergePolicyTest, TakeIncomingAsShotPreservesCurrentAsShotBaseline) {
  ColorTempOp op;
  const nlohmann::json current = {
      {"color_temp",
       {{"mode", "custom"},
        {"custom_cct", 7000.0},
        {"custom_tint", 10.0},
        {"as_shot_cct", 5200.0},
        {"as_shot_tint", -3.0}}}};
  const nlohmann::json incoming = {{"color_temp", {{"mode", "as_shot"}}}};
  const auto merged =
      op.MergeParams(current, incoming, OperatorMergeChoice::kTakeIncoming);
  ASSERT_TRUE(merged.contains("color_temp"));
  const auto& ct = merged["color_temp"];
  EXPECT_EQ(ct.value("mode", std::string{}), "as_shot");
  EXPECT_DOUBLE_EQ(ct.value("as_shot_cct", 0.0), 5200.0);
  EXPECT_DOUBLE_EQ(ct.value("as_shot_tint", 0.0), -3.0);
  EXPECT_DOUBLE_EQ(ct.value("custom_cct", 0.0), 7000.0);
  EXPECT_DOUBLE_EQ(ct.value("custom_tint", 0.0), 10.0);
}

TEST(ColorTempOpMergePolicyTest, SetParamsAsShotWithoutAsShotKeysKeepsExistingAsShot) {
  ColorTempOp op;
  op.SetParams({{"color_temp",
                 {{"mode", "custom"},
                  {"custom_cct", 7000.0},
                  {"custom_tint", 12.0},
                  {"as_shot_cct", 4800.0},
                  {"as_shot_tint", -5.0}}}});
  op.SetParams({{"color_temp", {{"mode", "as_shot"}}}});
  const auto params = op.GetParams()["color_temp"];
  EXPECT_EQ(params.value("mode", std::string{}), "as_shot");
  EXPECT_DOUBLE_EQ(params.value("as_shot_cct", 0.0), 4800.0);
  EXPECT_DOUBLE_EQ(params.value("as_shot_tint", 0.0), -5.0);
  EXPECT_DOUBLE_EQ(params.value("custom_cct", 0.0), 7000.0);
  EXPECT_DOUBLE_EQ(params.value("custom_tint", 0.0), 12.0);
}

TEST(ColorTempOpMergePolicyTest, SetParamsAcceptsLegacyResolvedAndCctKeys) {
  ColorTempOp op;
  op.SetParams({{"color_temp",
                 {{"mode", "custom"},
                  {"cct", 7000.0},
                  {"tint", 12.0},
                  {"resolved_cct", 4800.0},
                  {"resolved_tint", -5.0}}}});
  const auto params = op.GetParams()["color_temp"];
  EXPECT_DOUBLE_EQ(params.value("custom_cct", 0.0), 7000.0);
  EXPECT_DOUBLE_EQ(params.value("custom_tint", 0.0), 12.0);
  EXPECT_DOUBLE_EQ(params.value("as_shot_cct", 0.0), 4800.0);
  EXPECT_DOUBLE_EQ(params.value("as_shot_tint", 0.0), -5.0);
}

TEST(LensCalibOpMergePolicyTest, ImageLocalMetaDoesNotForceConflictWhenPortableMatches) {
  LensCalibOp op;
  const nlohmann::json current = {
      {"lens_calib",
       {{"enabled", true},
        {"apply_distortion", true},
        {"cam_maker", "Canon"},
        {"cam_model", "EOS R5"},
        {"lens_model", "RF 24-70"}}}};
  const nlohmann::json incoming = {
      {"lens_calib", {{"enabled", true}, {"apply_distortion", true}}}};
  EXPECT_FALSE(op.DetectMergeConflict(current, incoming));
}

TEST(LensCalibOpMergePolicyTest, TakeIncomingKeepsTargetImageLocalMeta) {
  LensCalibOp op;
  const nlohmann::json current = {
      {"lens_calib",
       {{"enabled", false},
        {"apply_distortion", true},
        {"cam_maker", "Nikon"},
        {"lens_model", "Target Lens"}}}};
  const nlohmann::json incoming = {
      {"lens_calib", {{"enabled", true}, {"apply_distortion", false}}}};
  const auto merged =
      op.MergeParams(current, incoming, OperatorMergeChoice::kTakeIncoming);
  const auto& lc = merged["lens_calib"];
  EXPECT_TRUE(lc.value("enabled", false));
  EXPECT_FALSE(lc.value("apply_distortion", true));
  EXPECT_EQ(lc.value("cam_maker", std::string{}), "Nikon");
  EXPECT_EQ(lc.value("lens_model", std::string{}), "Target Lens");
}

TEST_F(AdjustmentTransferPasteMergeTest,
       LivePipelineColorTempBothAsShotHasNoConflictDespiteStrippedIncoming) {
  // Live operator DetectMergeConflict (paste field-conflict policy).
  const auto element_id = test::EditorMiniGitProjectFixture::kElementA;

  auto guard = pipeline_service_->LoadPipeline(element_id);
  ASSERT_TRUE(guard && guard->pipeline_);
  nlohmann::json current_value;
  {
    std::unique_lock<std::mutex> lock(guard->pipeline_->GetRenderLock());
    auto&                        to_ws = guard->pipeline_->GetStage(PipelineStageName::To_WorkingSpace);
    const nlohmann::json full_as_shot = {
        {"color_temp",
         {{"mode", "as_shot"},
          {"custom_cct", 5123.0},
          {"custom_tint", -7.5},
          {"as_shot_cct", 5123.0},
          {"as_shot_tint", -7.5}}}};
    to_ws.SetOperator(OperatorType::COLOR_TEMP, full_as_shot, guard->pipeline_->GetGlobalParams());
    const auto current = to_ws.GetOperator(OperatorType::COLOR_TEMP);
    ASSERT_TRUE(current.has_value() && current.value() && current.value()->op_);
    current_value = current.value()->op_->GetParams();
    EXPECT_FALSE(current.value()->op_->DetectMergeConflict(
        current_value, nlohmann::json{{"color_temp", {{"mode", "as_shot"}}}}));
  }
  pipeline_service_->SavePipeline(guard);
}

TEST_F(AdjustmentTransferPasteMergeTest,
       LivePipelineColorTempTakeIncomingAsShotKeepsTargetResolvedBaseline) {
  const auto element_id = test::EditorMiniGitProjectFixture::kElementA;

  auto guard = pipeline_service_->LoadPipeline(element_id);
  ASSERT_TRUE(guard && guard->pipeline_);
  nlohmann::json resolved;
  {
    std::unique_lock<std::mutex> lock(guard->pipeline_->GetRenderLock());
    auto&                        to_ws = guard->pipeline_->GetStage(PipelineStageName::To_WorkingSpace);
    const nlohmann::json custom = {
        {"color_temp",
         {{"mode", "custom"},
          {"custom_cct", 7000.0},
          {"custom_tint", 15.0},
          {"as_shot_cct", 4550.0},
          {"as_shot_tint", -2.0}}}};
    to_ws.SetOperator(OperatorType::COLOR_TEMP, custom, guard->pipeline_->GetGlobalParams());
    const auto current = to_ws.GetOperator(OperatorType::COLOR_TEMP);
    ASSERT_TRUE(current.has_value() && current.value() && current.value()->op_);
    const auto current_value = current.value()->op_->GetParams();
    const nlohmann::json incoming = {{"color_temp", {{"mode", "as_shot"}}}};
    ASSERT_TRUE(current.value()->op_->DetectMergeConflict(current_value, incoming));
    resolved = current.value()->op_->MergeParams(current_value, incoming,
                                                 OperatorMergeChoice::kTakeIncoming);
  }
  pipeline_service_->SavePipeline(guard);

  const auto& ct = resolved["color_temp"];
  EXPECT_EQ(ct.value("mode", std::string{}), "as_shot");
  EXPECT_DOUBLE_EQ(ct.value("as_shot_cct", 0.0), 4550.0);
  EXPECT_DOUBLE_EQ(ct.value("as_shot_tint", 0.0), -2.0);
}
}  // namespace
}  // namespace alcedo
