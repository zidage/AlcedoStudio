//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_session_history_port.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "app/adjustment_transfer_types.hpp"
#include "app/document_transfer.hpp"
#include "app/editor_pipeline_command_service.hpp"
#include "app/editor_render_intent.hpp"
#include "app/pipeline_service.hpp"
#include "edit/graph/adjustment_ownership.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/commit_types.hpp"
#include "edit/history/edit_commit.hpp"
#include "edit/history/pipeline_edit_batch.hpp"
#include "edit/mask/mask_asset.hpp"
#include "edit/mask/mask_id.hpp"
#include "edit/mask/mask_store.hpp"
#include "edit/operators/models/operator_type_id.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"
#include "grade_owned_mask_support.hpp"
#include "support/document_transfer_test_support.hpp"
#include "ui/alcedo_main/album_backend/editor_session_pipeline_port.hpp"

namespace alcedo::ui {
namespace {

auto MakePastePipelineGuard(sl_element_id_t element_id) -> std::shared_ptr<alcedo::PipelineGuard> {
  auto guard       = std::make_shared<alcedo::PipelineGuard>();
  guard->id_       = element_id;
  guard->pipeline_ = std::make_shared<alcedo::CPUPipelineExecutor>();
  guard->document_ =
      std::make_shared<alcedo::PipelineDocument>(alcedo::CreateDefaultPipelineDocument());
  guard->commit_graph_ =
      std::make_shared<alcedo::CommitGraph>(alcedo::CommitGraph::CreateEmpty(element_id));
  guard->root_id_ = guard->commit_graph_->GetRootId();
  guard->root_document_ =
      std::make_shared<alcedo::PipelineDocument>(alcedo::ClonePipelineDocument(*guard->document_));
  return guard;
}

class EditorDocumentPasteTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RegisterAllOperators();
    const auto stamp =
        std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    journal_path_ = std::filesystem::temp_directory_path() / ("document_paste_" + stamp + ".wal");
    guard_        = MakePastePipelineGuard(77);
    pipeline_     = std::make_shared<EditorSessionPipelinePort>();
    pipeline_->SetServices(
        EditorSessionPipelineMappers{{}, [guard = guard_](sl_element_id_t) { return guard; }});
    history_.SetServices(
        EditorSessionHistoryPort::Services{[this](sl_element_id_t) { return journal_path_; }});
    history_.SetPipelinePort(pipeline_);
  }

  void TearDown() override {
    history_.Release({77, true});
    alcedo::SetDocumentTransferIdentitySourceForTesting(nullptr);
    std::error_code ec;
    std::filesystem::remove(journal_path_, ec);
  }

  std::filesystem::path                      journal_path_;
  std::shared_ptr<alcedo::PipelineGuard>     guard_;
  std::shared_ptr<EditorSessionPipelinePort> pipeline_;
  EditorSessionHistoryPort                   history_;
};

TEST_F(EditorDocumentPasteTest, PasteCreatesOneRootRelativeVersionAndOneTypedCommit) {
  std::string error;
  const auto  handle = history_.Acquire(77, &error);
  ASSERT_TRUE(handle.valid) << error;

  const auto prior_version = guard_->commit_graph_->GetActiveVersionId();
  const auto prior_count   = guard_->commit_graph_->CommitCount();
  const auto prior_refs    = guard_->commit_graph_->GetAllVersionRefs().size();
  const auto package       = test::MakeExposureTransferPackage(0.85);

  alcedo::AdjustmentPasteResult paste_result;
  ASSERT_TRUE(history_.PasteLiveRootRelativeVersion(handle, package, "Pasted Typed", &paste_result,
                                                    &error))
      << error;
  ASSERT_TRUE(paste_result.pasted);
  EXPECT_EQ(paste_result.prior_version_id, prior_version);
  EXPECT_NE(guard_->commit_graph_->GetActiveVersionId(), prior_version);
  EXPECT_EQ(guard_->commit_graph_->GetActiveVersionId(), paste_result.new_version_id);
  EXPECT_EQ(guard_->commit_graph_->GetAllVersionRefs().size(), prior_refs + 1u);
  EXPECT_EQ(guard_->commit_graph_->CommitCount(), prior_count + 1u);
  ASSERT_TRUE(guard_->working_head_commit_hash().has_value());
  EXPECT_EQ(*guard_->working_head_commit_hash(), paste_result.new_head);

  const auto chain = guard_->commit_graph_->FirstParentChain(paste_result.new_head);
  ASSERT_EQ(chain.size(), 1u);
  const auto& commit = guard_->commit_graph_->GetCommit(chain[0]);
  EXPECT_EQ(commit.GetKind(), alcedo::EditCommitKind::kEdit);
  EXPECT_EQ(commit.GetFirstParentHash(), std::nullopt);
  EXPECT_EQ(commit.GetSecondParentHash(), std::nullopt);
  ASSERT_TRUE(alcedo::IsPipelineEditBatchJson(commit.GetPayloadJSON()));
  const auto batch = alcedo::PipelineEditBatch::FromJSON(commit.GetPayloadJSON());
  EXPECT_EQ(batch.operation_kind, alcedo::PipelineEditOperationKind::Paste);
  EXPECT_EQ(history_.LastPublishedRenderReason(), alcedo::EditorRenderReason::PastedPipelineDocument);
}

TEST_F(EditorDocumentPasteTest, FailedPasteCreatesNoVersionCommitHeadMoveOrRender) {
  std::string error;
  const auto  handle = history_.Acquire(77, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto prior_version = guard_->commit_graph_->GetActiveVersionId();
  const auto prior_count   = guard_->commit_graph_->CommitCount();
  const auto prior_refs    = guard_->commit_graph_->GetAllVersionRefs().size();
  const auto prior_head    = guard_->working_head_commit_hash();
  const auto prior_reason  = history_.LastPublishedRenderReason();
  const auto prior_hash    = alcedo::CanonicalPipelineDocumentJson(*guard_->document_);

  alcedo::AdjustmentPasteResult empty_result;
  EXPECT_FALSE(history_.PasteLiveRootRelativeVersion(
      handle, alcedo::AdjustmentTransferPackage{}, "Empty", &empty_result, &error));
  EXPECT_FALSE(empty_result.pasted);

  class CollidingIdentity final : public alcedo::TransferIdentitySource {
   public:
    auto NextNodeId() -> alcedo::NodeId override { return alcedo::NodeId{"grade.primary"}; }
    auto NextAdjustmentInstanceId(const alcedo::NodeId& node_id,
                                  const alcedo::OperatorTypeId& type)
        -> alcedo::AdjustmentInstanceId override {
      return alcedo::MakeAdjustmentInstanceId(node_id, type);
    }
    auto NextMaskId() -> alcedo::MaskId override { return alcedo::MaskId{"mask.t1"}; }
  } colliding;
  alcedo::SetDocumentTransferIdentitySourceForTesting(&colliding);
  alcedo::AdjustmentPasteResult collision_result;
  EXPECT_FALSE(history_.PasteLiveRootRelativeVersion(
      handle, test::MakeExposureTransferPackage(1.0), "Collision", &collision_result, &error));
  EXPECT_FALSE(collision_result.pasted);
  alcedo::SetDocumentTransferIdentitySourceForTesting(nullptr);

  const auto mask_root =
      std::filesystem::path{"build/tmp/node_history"} / "failed_paste_missing_asset";
  std::error_code ignored;
  std::filesystem::remove_all(mask_root, ignored);
  alcedo::MaskStore source_store(mask_root);
  alcedo::MaskAssetDescriptor descriptor;
  descriptor.extent           = {2, 2};
  descriptor.reference_bounds = {0.0f, 0.0f, 1.0f, 1.0f};
  const std::vector<std::uint8_t> pixels(4, 73);
  const auto                      key = source_store.Put(descriptor, pixels);
  auto                            masked = test::DocumentWithExposureEv(0.4);
  grade_mask_test::AddBrushMask(masked, alcedo::MaskId{"mask.brush"}, key, descriptor);
  const auto missing_asset_package = alcedo::CaptureDocumentTransfer(masked, &source_store);
  alcedo::AdjustmentPasteResult missing_result;
  EXPECT_FALSE(history_.PasteLiveRootRelativeVersion(handle, missing_asset_package, "Missing asset",
                                                     &missing_result, &error));
  EXPECT_FALSE(missing_result.pasted);

  EXPECT_EQ(guard_->commit_graph_->GetActiveVersionId(), prior_version);
  EXPECT_EQ(guard_->commit_graph_->CommitCount(), prior_count);
  EXPECT_EQ(guard_->commit_graph_->GetAllVersionRefs().size(), prior_refs);
  EXPECT_EQ(guard_->working_head_commit_hash(), prior_head);
  EXPECT_EQ(history_.LastPublishedRenderReason(), prior_reason);
  EXPECT_EQ(alcedo::CanonicalPipelineDocumentJson(*guard_->document_), prior_hash);
}

TEST_F(EditorDocumentPasteTest, FailedPasteWalAppendCreatesNoVersionCommitHeadMoveOrRender) {
  history_.SetServices(
      EditorSessionHistoryPort::Services{[bad = journal_path_.parent_path() / "not-a-directory"](
                                             sl_element_id_t) { return bad / "image-77.wal"; }});
  {
    std::ofstream blocker(journal_path_.parent_path() / "not-a-directory", std::ios::binary);
    ASSERT_TRUE(blocker.is_open());
    blocker << "block";
  }
  std::string error;
  const auto  handle = history_.Acquire(77, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto prior_version = guard_->commit_graph_->GetActiveVersionId();
  const auto prior_count   = guard_->commit_graph_->CommitCount();
  const auto prior_refs    = guard_->commit_graph_->GetAllVersionRefs().size();
  const auto prior_head    = guard_->working_head_commit_hash();
  const auto prior_reason  = history_.LastPublishedRenderReason();
  const auto prior_hash    = alcedo::CanonicalPipelineDocumentJson(*guard_->document_);

  alcedo::AdjustmentPasteResult paste_result;
  EXPECT_FALSE(history_.PasteLiveRootRelativeVersion(
      handle, test::MakeExposureTransferPackage(1.25), "WAL fail", &paste_result, &error));
  EXPECT_FALSE(paste_result.pasted);
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(guard_->commit_graph_->GetActiveVersionId(), prior_version);
  EXPECT_EQ(guard_->commit_graph_->CommitCount(), prior_count);
  EXPECT_EQ(guard_->commit_graph_->GetAllVersionRefs().size(), prior_refs);
  EXPECT_EQ(guard_->working_head_commit_hash(), prior_head);
  EXPECT_EQ(history_.LastPublishedRenderReason(), prior_reason);
  EXPECT_EQ(alcedo::CanonicalPipelineDocumentJson(*guard_->document_), prior_hash);

  std::error_code ec;
  std::filesystem::remove(journal_path_.parent_path() / "not-a-directory", ec);
}

TEST_F(EditorDocumentPasteTest, TransferSurfaceHasNoPipelineMergeOperation) {
  std::string error;
  const auto  handle = history_.Acquire(77, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto package = test::MakeExposureTransferPackage(1.25);
  alcedo::AdjustmentMergePreview preview;
  EXPECT_FALSE(history_.BeginLiveMerge(handle, package, &preview, &error));
  EXPECT_NE(error.find("Pipeline merge is not supported"), std::string::npos);
  EXPECT_EQ(guard_->commit_graph_->CommitCount(), 0u);

  alcedo::AdjustmentMergeResult merge_result;
  EXPECT_FALSE(history_.CompleteLiveMerge(handle, package, preview, {}, &merge_result, &error));
  EXPECT_FALSE(merge_result.merged);
  EXPECT_EQ(guard_->commit_graph_->CommitCount(), 0u);
  EXPECT_FALSE(guard_->working_head_commit_hash().has_value());
}

}  // namespace
}  // namespace alcedo::ui
