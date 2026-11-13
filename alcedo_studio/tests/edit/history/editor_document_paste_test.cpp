//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "app/adjustment_transfer_types.hpp"
#include "app/document_transfer.hpp"
#include "app/document_transfer_planner.hpp"
#include "app/editor_pipeline_command_service.hpp"
#include "app/editor_render_intent.hpp"
#include "app/pipeline_service.hpp"
#include "edit/graph/adjustment_ownership.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_snapshot.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/commit_types.hpp"
#include "edit/history/edit_commit.hpp"
#include "edit/history/pipeline_edit_batch.hpp"
#include "edit/mask/mask_id.hpp"
#include "edit/operators/models/operator_type_id.hpp"
#include "grade_owned_mask_support.hpp"
#include "support/document_transfer_test_support.hpp"
#include "support/editor_history_port_test_reads.hpp"
#include "support/editor_lease_test_support.hpp"
#include "ui/alcedo_main/album_backend/editor_session_history_port.hpp"
#include "ui/alcedo_main/album_backend/editor_session_pipeline_port.hpp"

namespace alcedo::ui {
namespace {

class EditorDocumentPasteTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto stamp =
        std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    journal_path_ = std::filesystem::temp_directory_path() / ("document_paste_" + stamp + ".wal");
    pipeline_     = std::make_shared<EditorSessionPipelinePort>();
    // The root carries the working-space camera profile, as every stored root does, so a
    // checkout of a root Version reproduces the opened document.
    pipeline_->SetServices(
        EditorSessionPipelineMappers{{}, [](sl_element_id_t id) {
                                       return alcedo::test::MakeInMemoryEditorLease(
                                           id, alcedo::test::WorkingSpaceBoundDefaultDocument());
                                     }});
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

  auto Graph() -> std::shared_ptr<const alcedo::CommitGraph> {
    return alcedo::test::EditorHistoryGraph(history_, 77);
  }
  auto Head() -> alcedo::head_commit_hash_t {
    return alcedo::test::EditorWorkingHead(history_, 77);
  }
  auto Working() -> std::shared_ptr<const alcedo::PipelineGraphSnapshot> {
    return alcedo::test::EditorWorkingPreview(*pipeline_, 77);
  }

  std::filesystem::path                      journal_path_;
  std::shared_ptr<EditorSessionPipelinePort> pipeline_;
  EditorSessionHistoryPort                   history_;
};

TEST_F(EditorDocumentPasteTest, PasteCreatesOneRootRelativeVersionAndOneTypedCommit) {
  std::string error;
  const auto  handle = history_.Acquire(77, &error);
  ASSERT_TRUE(handle.valid) << error;

  const auto prior_version     = Graph()->GetActiveVersionId();
  const auto prior_count       = Graph()->CommitCount();
  const auto prior_refs        = Graph()->GetAllVersionRefs().size();
  auto       source            = test::DocumentWithExposureEv(0.85);
  const auto source_default_id = source.DefaultGradeId();
  const auto target_default_id = Working()->Document().DefaultGradeId();
  const auto target_document   = alcedo::CanonicalPipelineDocumentJson(Working()->Document());
  source.PrimaryGrade()->SetDisplayName("Renamed source default");
  source.PrimaryGrade()->SetDeletionProtected(false);
  const alcedo::MaskId unlocked{"mask.unlocked"};
  const alcedo::MaskId locked{"mask.locked"};
  alcedo::grade_mask_test::AddRadialMask(source, unlocked);
  source.PrimaryGrade()->SetMaskDisplayName(unlocked, "Unlocked source Mask");
  source.PrimaryGrade()->SetMaskDeletionProtected(unlocked, false);
  alcedo::grade_mask_test::AddLinearGradientMask(source, locked);
  source.PrimaryGrade()->SetMaskDisplayName(locked, "Locked source Mask");
  source.PrimaryGrade()->SetMaskDeletionProtected(locked, true);
  const auto                    package = alcedo::CaptureDocumentTransfer(source);

  alcedo::AdjustmentPasteResult paste_result;
  ASSERT_TRUE(
      history_.PasteLiveRootRelativeVersion(handle, package, "Pasted Typed", &paste_result, &error))
      << error;
  ASSERT_TRUE(paste_result.pasted);
  EXPECT_EQ(paste_result.prior_version_id, prior_version);
  EXPECT_NE(Graph()->GetActiveVersionId(), prior_version);
  EXPECT_EQ(Graph()->GetActiveVersionId(), paste_result.new_version_id);
  EXPECT_EQ(Graph()->GetAllVersionRefs().size(), prior_refs + 1u);
  EXPECT_EQ(Graph()->CommitCount(), prior_count + 1u);
  ASSERT_TRUE(Head().has_value());
  EXPECT_EQ(*Head(), paste_result.new_head);

  const auto graph = Graph();
  const auto chain = graph->FirstParentChain(paste_result.new_head);
  ASSERT_EQ(chain.size(), 1u);
  const auto& commit = graph->GetCommit(chain[0]);
  EXPECT_EQ(commit.GetFirstParentHash(), std::nullopt);
  ASSERT_TRUE(alcedo::IsPipelineEditBatchJson(commit.GetPayloadJSON()));
  const auto batch = alcedo::PipelineEditBatch::FromJSON(commit.GetPayloadJSON());
  EXPECT_EQ(batch.operation_kind, alcedo::PipelineEditOperationKind::Paste);
  EXPECT_EQ(history_.LastPublishedRenderReason(),
            alcedo::EditorRenderReason::PastedPipelineDocument);

  const auto pasted_default_id = Working()->Document().DefaultGradeId();
  EXPECT_FALSE(pasted_default_id.Empty());
  EXPECT_NE(pasted_default_id, source_default_id);
  const auto assert_pasted_state = [&]() {
    EXPECT_EQ(Working()->Document().DefaultGradeId(), pasted_default_id);
    const auto  working = Working();
    const auto* grade   = dynamic_cast<const alcedo::ColorGradeNodeModel*>(
        working->Document().Graph().FindNode(pasted_default_id));
    ASSERT_NE(grade, nullptr);
    EXPECT_EQ(grade->DisplayName(), "Renamed source default");
    // The remapped source default stays the target default, so the target
    // default protection rule applies to the Grade. Masks keep their source
    // flags verbatim: Mask-level locks no longer exist.
    EXPECT_TRUE(grade->DeletionProtected());
    ASSERT_EQ(grade->MaskCount(), 2u);
    EXPECT_EQ(grade->MaskAt(0).display_name, "Unlocked source Mask");
    EXPECT_FALSE(grade->MaskAt(0).deletion_protected);
    EXPECT_NE(grade->MaskAt(0).id, alcedo::MaskId{"mask.unlocked"});
    EXPECT_EQ(grade->MaskAt(1).display_name, "Locked source Mask");
    EXPECT_TRUE(grade->MaskAt(1).deletion_protected);
    EXPECT_NE(grade->MaskAt(1).id, alcedo::MaskId{"mask.locked"});
  };
  assert_pasted_state();
  const auto pasted_document = alcedo::CanonicalPipelineDocumentJson(Working()->Document());
  alcedo::version_ref_id_t clean_version{};
  ASSERT_TRUE(history_.CreateRootVersionAndCheckout(handle, "Clean target", &clean_version, &error))
      << error;
  EXPECT_EQ(Working()->Document().DefaultGradeId(), target_default_id);
  EXPECT_EQ(alcedo::CanonicalPipelineDocumentJson(Working()->Document()), target_document);
  ASSERT_TRUE(history_.CheckoutVersion(handle, paste_result.new_version_id, &error)) << error;
  assert_pasted_state();
  EXPECT_EQ(alcedo::CanonicalPipelineDocumentJson(Working()->Document()), pasted_document);
}

TEST_F(EditorDocumentPasteTest, PasteWithoutSourceDefaultEstablishesFirstGradeAsDefault) {
  std::string error;
  const auto  handle = history_.Acquire(77, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto target_default_id = Working()->Document().DefaultGradeId();
  ASSERT_FALSE(target_default_id.Empty());
  auto source = test::DocumentWithExposureEv(0.5);
  source.SetDefaultGradeId(alcedo::NodeId{});
  source.PrimaryGrade()->SetDeletionProtected(false);
  const auto                    package = alcedo::CaptureDocumentTransfer(source);
  alcedo::AdjustmentPasteResult result;
  ASSERT_TRUE(history_.PasteLiveRootRelativeVersion(handle, package, "No default", &result, &error))
      << error;
  ASSERT_TRUE(result.pasted);
  // The package omits the source default, so the first included Grade becomes
  // the target default and receives the required default deletion protection.
  const auto pasted_default_id = Working()->Document().DefaultGradeId();
  EXPECT_FALSE(pasted_default_id.Empty());
  EXPECT_NE(pasted_default_id, target_default_id);
  const auto  pasted  = Working();
  const auto* primary = pasted->Document().PrimaryGrade();
  ASSERT_NE(primary, nullptr);
  EXPECT_EQ(primary->Id(), pasted_default_id);
  EXPECT_TRUE(primary->DeletionProtected());
  const auto pasted_document = alcedo::CanonicalPipelineDocumentJson(Working()->Document());
  alcedo::version_ref_id_t clean_version{};
  ASSERT_TRUE(history_.CreateRootVersionAndCheckout(handle, "Clean target", &clean_version, &error))
      << error;
  EXPECT_EQ(Working()->Document().DefaultGradeId(), target_default_id);
  ASSERT_TRUE(history_.CheckoutVersion(handle, result.new_version_id, &error)) << error;
  EXPECT_EQ(Working()->Document().DefaultGradeId(), pasted_default_id);
  EXPECT_EQ(alcedo::CanonicalPipelineDocumentJson(Working()->Document()), pasted_document);
}

TEST_F(EditorDocumentPasteTest, FailedPasteCreatesNoVersionCommitHeadMoveOrRender) {
  std::string error;
  const auto  handle = history_.Acquire(77, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto prior_version = Graph()->GetActiveVersionId();
  const auto prior_count   = Graph()->CommitCount();
  const auto prior_refs    = Graph()->GetAllVersionRefs().size();
  const auto prior_head    = Head();
  const auto prior_reason  = history_.LastPublishedRenderReason();
  const auto prior_hash    = alcedo::CanonicalPipelineDocumentJson(Working()->Document());
  const auto prior_lineage = Working()->Lineage();

  alcedo::AdjustmentPasteResult empty_result;
  EXPECT_FALSE(history_.PasteLiveRootRelativeVersion(handle, alcedo::AdjustmentTransferPackage{},
                                                     "Empty", &empty_result, &error));
  EXPECT_FALSE(empty_result.pasted);

  class CollidingIdentity final : public alcedo::TransferIdentitySource {
   public:
    auto NextNodeId() -> alcedo::NodeId override { return alcedo::NodeId{"grade.primary"}; }
    auto NextAdjustmentInstanceId(const alcedo::NodeId& node_id, const alcedo::OperatorTypeId& type)
        -> alcedo::AdjustmentInstanceId override {
      return alcedo::MakeAdjustmentInstanceId(node_id, type);
    }
    auto NextMaskId() -> alcedo::MaskId override { return alcedo::MaskId{"mask.t1"}; }
#ifdef ALCEDO_ENABLE_BRUSH_MASK
    auto NextStrokeId() -> alcedo::StrokeId override { return alcedo::StrokeId{"stroke.t1"}; }
#endif
  } colliding;
  alcedo::SetDocumentTransferIdentitySourceForTesting(&colliding);
  alcedo::AdjustmentPasteResult collision_result;
  EXPECT_FALSE(history_.PasteLiveRootRelativeVersion(handle, test::MakeExposureTransferPackage(1.0),
                                                     "Collision", &collision_result, &error));
  EXPECT_FALSE(collision_result.pasted);
  alcedo::SetDocumentTransferIdentitySourceForTesting(nullptr);

  EXPECT_EQ(Graph()->GetActiveVersionId(), prior_version);
  EXPECT_EQ(Graph()->CommitCount(), prior_count);
  EXPECT_EQ(Graph()->GetAllVersionRefs().size(), prior_refs);
  EXPECT_EQ(Head(), prior_head);
  EXPECT_EQ(history_.LastPublishedRenderReason(), prior_reason);
  EXPECT_EQ(alcedo::CanonicalPipelineDocumentJson(Working()->Document()), prior_hash);
  // Build-then-swap: a failed Paste never binds its built document.
  EXPECT_EQ(Working()->Lineage(), prior_lineage);
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
  const auto prior_version = Graph()->GetActiveVersionId();
  const auto prior_count   = Graph()->CommitCount();
  const auto prior_refs    = Graph()->GetAllVersionRefs().size();
  const auto prior_head    = Head();
  const auto prior_reason  = history_.LastPublishedRenderReason();
  const auto prior_hash    = alcedo::CanonicalPipelineDocumentJson(Working()->Document());
  const auto prior_lineage = Working()->Lineage();

  alcedo::AdjustmentPasteResult paste_result;
  EXPECT_FALSE(history_.PasteLiveRootRelativeVersion(
      handle, test::MakeExposureTransferPackage(1.25), "WAL fail", &paste_result, &error));
  EXPECT_FALSE(paste_result.pasted);
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(Graph()->GetActiveVersionId(), prior_version);
  EXPECT_EQ(Graph()->CommitCount(), prior_count);
  EXPECT_EQ(Graph()->GetAllVersionRefs().size(), prior_refs);
  EXPECT_EQ(Head(), prior_head);
  EXPECT_EQ(history_.LastPublishedRenderReason(), prior_reason);
  EXPECT_EQ(alcedo::CanonicalPipelineDocumentJson(Working()->Document()), prior_hash);
  // Build-then-swap: a failed Paste never binds its built document.
  EXPECT_EQ(Working()->Lineage(), prior_lineage);

  std::error_code ec;
  std::filesystem::remove(journal_path_.parent_path() / "not-a-directory", ec);
}

}  // namespace
}  // namespace alcedo::ui
