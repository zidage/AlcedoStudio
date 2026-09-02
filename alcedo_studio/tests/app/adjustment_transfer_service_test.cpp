//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/adjustment_transfer_service.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/commit_types.hpp"
#include "edit/history/edit_commit.hpp"
#include "edit/history/pipeline_edit_batch.hpp"
#include "edit/history/pipeline_history_format.hpp"
#include "json.hpp"
#include "support/document_transfer_test_support.hpp"
#include "support/editor_mini_git_project_fixture.hpp"

namespace alcedo {
namespace {

TEST(AdjustmentTransferServiceTest, CaptureExportImportRoundTripOmitsDevelopAndGeometry) {
  auto source = test::DocumentWithExposureEv(1.5);
  source.Geometry().SetRotationDegrees(12.0f);
  const auto package  = AdjustmentTransferService::Capture(source);
  const auto exported = AdjustmentTransferService::ExportPackage(package);
  EXPECT_FALSE(exported.contains("operators"));
  EXPECT_FALSE(exported.contains("develop"));
  EXPECT_FALSE(exported.contains("geometry"));
  const auto imported = AdjustmentTransferService::ImportPackage(exported);
  EXPECT_EQ(imported.fingerprint_, package.fingerprint_);
  EXPECT_FALSE(imported.Empty());
}

TEST(AdjustmentTransferServiceTest, ImportRejectsOperatorListPackages) {
  const nlohmann::json json = {{"operators", nlohmann::json::array({nlohmann::json::object()})},
                               {"schema", std::string{kAdjustmentTransferSchema}}};
  EXPECT_THROW((void)AdjustmentTransferService::ImportPackage(json), std::runtime_error);
}

TEST(AdjustmentTransferServiceTest, TransferSurfaceHasNoPipelineMergeOperation) {
  test::EditorMiniGitProjectFixture project;
  project.SetUp();
  auto* graph = project.graph(test::EditorMiniGitProjectFixture::kElementA).get();
  ASSERT_NE(graph, nullptr);
  const auto package = test::MakeDefaultDocumentTransferPackage();
  const auto pasted  = AdjustmentTransferService::PasteAsRootRelativeVersion(
      *graph, CreateDefaultPipelineDocument(), package, "Pasted Adjustments");
  ASSERT_TRUE(pasted.pasted) << pasted.error;
  EXPECT_EQ(graph->CommitCount(), 1u);
  const auto& commit = graph->GetCommit(pasted.new_head);
  EXPECT_EQ(commit.GetKind(), EditCommitKind::kEdit);
  EXPECT_EQ(commit.GetSecondParentHash(), std::nullopt);
  ASSERT_TRUE(IsPipelineEditBatchJson(commit.GetPayloadJSON()));
  EXPECT_EQ(PipelineEditBatch::FromJSON(commit.GetPayloadJSON()).operation_kind,
            PipelineEditOperationKind::Paste);
  for (const auto& [id, ref] : graph->GetAllVersionRefs()) {
    (void)id;
    if (!ref.head_commit_hash.has_value()) {
      continue;
    }
    EXPECT_NE(graph->GetCommit(*ref.head_commit_hash).GetKind(), EditCommitKind::kMerge);
  }
  project.TearDown();
}

}  // namespace
}  // namespace alcedo
