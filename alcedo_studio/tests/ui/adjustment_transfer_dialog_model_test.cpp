//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file adjustment_transfer_dialog_model_test.cpp
/// @brief NM10.4 dialog-model tests: stable identity roles, derived three-state
///        checks, focus/selection independence, command behavior, and
///        fail-closed Version replay.

#include "ui/alcedo_main/album_backend/adjustment_transfer_dialog_model.hpp"

#include <gtest/gtest.h>

#include <QAbstractItemModel>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>

#include "app/editor_pipeline_command_service.hpp"
#include "app/pipeline_document_history.hpp"
#include "app/pipeline_history_applier.hpp"
#include "edit/graph/adjustment_ownership.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/grade_owned_mask_support.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_commands.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "edit/history/pipeline_edit_batch.hpp"
#include "edit/mask/mask_id.hpp"
#include "edit/operators/models/adjustment_catalog.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "support/document_transfer_test_support.hpp"
#include "support/editor_parameter_target_test.hpp"

namespace alcedo::ui::test {
namespace {

/// Journal + commit + head move first, then the live-document apply — the same
/// publish order the production edit path uses.
class DialogModelHistoryFixture {
 public:
  DialogModelHistoryFixture() {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_             = std::filesystem::path{"build"} / "tmp" / "adjustment_transfer_dialog_model" /
           std::string{info->name()};
    std::filesystem::create_directories(dir_);
    journal_path_ = dir_ / "image.wal";
    std::error_code ec;
    std::filesystem::remove(journal_path_, ec);

    journal_       = std::make_shared<MiniGitJournal>(journal_path_);
    graph_         = std::make_shared<CommitGraph>(CommitGraph::CreateEmpty(41));
    history_       = std::make_unique<MiniGitWorkingHistory>(graph_, journal_);
    root_document_ = std::make_shared<PipelineDocument>(CreateDefaultPipelineDocument());
  }

  auto CommitBatch(const PipelineEditBatch& batch) -> MiniGitEditAppendResult {
    const auto result = history_->AppendEdit(batch);
    if (result.committed) {
      std::string error;
      EXPECT_TRUE(ApplyPipelineEditBatch(live_document_, batch, PipelineEditApplyDirection::Forward,
                                         &error))
          << error;
    }
    return result;
  }

  auto CommitExposure(double before, double after) -> MiniGitEditAppendResult {
    return CommitBatch(MakeSetParameterBatch(alcedo::test::ColorGradeFieldTarget("exposure"),
                                             {{"exposure_ev", before}}, {{"exposure_ev", after}},
                                             true, true, "Color Grade 1"));
  }

  /// PipelineDocument is move-only (unique_ptr nodes): rebuild the shared
  /// root instead of copy-assigning into it.
  void ReplaceRootDocument(PipelineDocument document) {
    root_document_ = std::make_shared<PipelineDocument>(std::move(document));
  }

  std::filesystem::path                  dir_;
  std::filesystem::path                  journal_path_;
  std::shared_ptr<MiniGitJournal>        journal_;
  std::shared_ptr<CommitGraph>           graph_;
  std::unique_ptr<MiniGitWorkingHistory> history_;
  std::shared_ptr<PipelineDocument>      root_document_;
  /// Live document at the working tip; the model must never read or mutate it.
  PipelineDocument                       live_document_ = CreateDefaultPipelineDocument();
};

auto RowValue(const QAbstractItemModel* model, int row, int role) -> QVariant {
  return model->data(model->index(row, 0), role);
}

auto RowText(const QAbstractItemModel* model, int row, int role) -> QString {
  return RowValue(model, row, role).toString();
}

/// Number of items in the focused node's item model with checked == true.
auto CheckedItemCount(const QAbstractItemModel* model) -> int {
  int count = 0;
  for (int row = 0; row < model->rowCount(); ++row) {
    if (RowValue(model, row, AdjustmentTransferItemListModel::CheckedRole).toBool()) {
      ++count;
    }
  }
  return count;
}

auto ItemRowForKey(const QAbstractItemModel* model, const QString& key) -> int {
  for (int row = 0; row < model->rowCount(); ++row) {
    if (RowText(model, row, AdjustmentTransferItemListModel::ItemKeyRole) == key) {
      return row;
    }
  }
  return -1;
}

auto SelectionFingerprint(const AdjustmentTransferDialogModel& model) -> std::string {
  std::string error;
  const auto  package = model.BuildPackage(&error);
  if (!package.has_value()) {
    return "<no-package:" + error + ">";
  }
  return package->fingerprint_;
}

auto SelectionShape(const AdjustmentTransferDialogModel& model) -> std::string {
  const auto  selection = model.BuildSelection();
  std::string shape;
  for (const auto& node : selection.nodes) {
    shape += std::string{node.node_id.Value()} + ":" + std::to_string(node.items.size()) + ";";
  }
  return shape;
}

}  // namespace

// ============================================================================
// Version rows
// ============================================================================

TEST(AdjustmentTransferDialogModelTest, VersionRowsKeepStableIdsAndOrdering) {
  DialogModelHistoryFixture fixture;
  ASSERT_TRUE(fixture.CommitExposure(1.5, 2.0).committed);
  const auto created_after_default = fixture.graph_->GetActiveVersionRef().created_at + 1;
  const auto inactive_id =
      fixture.graph_->CreateVersionRefAtActiveHead("Warm Portrait", created_after_default);

  AdjustmentTransferDialogModel model;
  std::string                   error;
  ASSERT_TRUE(model.OpenSource(fixture.graph_, fixture.root_document_, &error)) << error;

  auto* versions = model.versions();
  ASSERT_NE(versions, nullptr);
  ASSERT_EQ(versions->rowCount(), 2);
  EXPECT_EQ(RowText(versions, 0, AdjustmentTransferVersionListModel::DisplayNameRole),
            QStringLiteral("Default"));
  EXPECT_TRUE(RowValue(versions, 0, AdjustmentTransferVersionListModel::ActiveRole).toBool());
  EXPECT_TRUE(RowValue(versions, 0, AdjustmentTransferVersionListModel::SelectedRole).toBool());
  EXPECT_FALSE(RowValue(versions, 1, AdjustmentTransferVersionListModel::SelectedRole).toBool());
  EXPECT_EQ(RowText(versions, 1, AdjustmentTransferVersionListModel::VersionIdRole),
            QString::fromStdString(inactive_id.ToString()));
  EXPECT_EQ(RowText(versions, 1, AdjustmentTransferVersionListModel::DisplayNameRole),
            QStringLiteral("Warm Portrait"));
}

// ============================================================================
// Node rows
// ============================================================================

TEST(AdjustmentTransferDialogModelTest, NodeRowsFollowSourceBackboneWithDrtPostLast) {
  DialogModelHistoryFixture fixture;
  auto                      document = CreateDefaultPipelineDocument();
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.extra"}).empty());
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"grade.extra"}, NodeId{"grade.first"}).empty());
  fixture.ReplaceRootDocument(std::move(document));

  AdjustmentTransferDialogModel model;
  std::string                   error;
  ASSERT_TRUE(model.OpenSource(fixture.graph_, fixture.root_document_, &error)) << error;

  auto* nodes = model.nodes();
  ASSERT_NE(nodes, nullptr);
  ASSERT_EQ(nodes->rowCount(), 4);
  EXPECT_EQ(RowText(nodes, 0, AdjustmentTransferNodeListModel::NodeIdRole),
            QStringLiteral("grade.primary"));
  EXPECT_EQ(RowText(nodes, 1, AdjustmentTransferNodeListModel::NodeIdRole),
            QStringLiteral("grade.first"));
  EXPECT_EQ(RowText(nodes, 2, AdjustmentTransferNodeListModel::NodeIdRole),
            QStringLiteral("grade.extra"));
  EXPECT_EQ(RowText(nodes, 3, AdjustmentTransferNodeListModel::NodeIdRole), QStringLiteral("drt"));
  EXPECT_EQ(RowValue(nodes, 3, AdjustmentTransferNodeListModel::NodeKindRole).toInt(),
            static_cast<int>(AdjustmentTransferNodeKind::DrtPost));
  // Every item starts checked; the first backbone node is focused.
  for (int row = 0; row < nodes->rowCount(); ++row) {
    EXPECT_EQ(RowValue(nodes, row, AdjustmentTransferNodeListModel::CheckStateRole).toInt(),
              Qt::Checked);
  }
  EXPECT_TRUE(RowValue(nodes, 0, AdjustmentTransferNodeListModel::FocusedRole).toBool());
  EXPECT_EQ(model.focused_node_id(), QStringLiteral("grade.primary"));
}

// ============================================================================
// Item identity
// ============================================================================

TEST(AdjustmentTransferDialogModelTest, ItemRowsUseStableAdjustmentInstanceIdentity) {
  DialogModelHistoryFixture fixture;
  auto                      document = CreateDefaultPipelineDocument();
  auto*                     grade    = document.PrimaryGrade();
  ASSERT_NE(grade, nullptr);
  auto extra = BuiltinAdjustmentCatalog::Instance().CreateDefault(type_ids::Exposure());
  ASSERT_NE(extra, nullptr);
  auto* extra_exposure = dynamic_cast<ExposureModel*>(extra.get());
  ASSERT_NE(extra_exposure, nullptr);
  extra_exposure->SetValue(-2.0f);
  document.InsertAdjustment(grade->Id(), grade->AdjustmentCount(),
                            AdjustmentInstanceId{"grade.primary.exposure.2"}, std::move(extra));
  fixture.ReplaceRootDocument(std::move(document));

  AdjustmentTransferDialogModel model;
  std::string                   error;
  ASSERT_TRUE(model.OpenSource(fixture.graph_, fixture.root_document_, &error)) << error;
  ASSERT_EQ(model.focused_node_id(), QStringLiteral("grade.primary"));

  auto* items = model.items();
  ASSERT_NE(items, nullptr);
  const int first  = ItemRowForKey(items, QStringLiteral("adj:grade.primary.exposure"));
  const int second = ItemRowForKey(items, QStringLiteral("adj:grade.primary.exposure.2"));
  ASSERT_GE(first, 0);
  ASSERT_GE(second, 0);
  EXPECT_NE(first, second);
  // Same type and label; distinct instance identities and values.
  EXPECT_EQ(RowText(items, first, AdjustmentTransferItemListModel::DisplayNameRole),
            RowText(items, second, AdjustmentTransferItemListModel::DisplayNameRole));
  EXPECT_NE(RowText(items, first, AdjustmentTransferItemListModel::DisplayValueRole),
            RowText(items, second, AdjustmentTransferItemListModel::DisplayValueRole));
}

// ============================================================================
// Masks row
// ============================================================================

TEST(AdjustmentTransferDialogModelTest, MasksRowIsSingleAllOrNoneItem) {
  DialogModelHistoryFixture fixture;
  auto                      document = CreateDefaultPipelineDocument();
  grade_mask_test::AddRadialMask(document, MaskId{"mask.a"});
  grade_mask_test::AddLinearGradientMask(document, MaskId{"mask.b"});
  grade_mask_test::AddRadialMask(document, MaskId{"mask.c"});
  fixture.ReplaceRootDocument(std::move(document));

  AdjustmentTransferDialogModel model;
  std::string                   error;
  ASSERT_TRUE(model.OpenSource(fixture.graph_, fixture.root_document_, &error)) << error;

  auto* items = model.items();
  ASSERT_NE(items, nullptr);
  int masks_rows = 0;
  int masks_row  = -1;
  for (int row = 0; row < items->rowCount(); ++row) {
    if (RowValue(items, row, AdjustmentTransferItemListModel::ItemKindRole).toInt() ==
        static_cast<int>(AdjustmentTransferItemKind::Masks)) {
      ++masks_rows;
      masks_row = row;
    }
  }
  ASSERT_EQ(masks_rows, 1);
  EXPECT_EQ(RowText(items, masks_row, AdjustmentTransferItemListModel::ItemKeyRole),
            QStringLiteral("masks"));
  EXPECT_EQ(RowText(items, masks_row, AdjustmentTransferItemListModel::DisplayValueRole),
            QStringLiteral("3"));
  EXPECT_TRUE(RowValue(items, masks_row, AdjustmentTransferItemListModel::EnabledRole).toBool());
}

TEST(AdjustmentTransferDialogModelTest, MasklessGradeShowsDisabledMasksItem) {
  DialogModelHistoryFixture     fixture;
  AdjustmentTransferDialogModel model;
  std::string                   error;
  ASSERT_TRUE(model.OpenSource(fixture.graph_, fixture.root_document_, &error)) << error;

  auto*     items = model.items();
  const int masks = ItemRowForKey(items, QStringLiteral("masks"));
  ASSERT_GE(masks, 0);
  EXPECT_FALSE(RowValue(items, masks, AdjustmentTransferItemListModel::EnabledRole).toBool());
  EXPECT_FALSE(RowValue(items, masks, AdjustmentTransferItemListModel::CheckedRole).toBool());
}

// ============================================================================
// Focus vs selection
// ============================================================================

TEST(AdjustmentTransferDialogModelTest, FocusingNodeDoesNotChangeTransferSelection) {
  DialogModelHistoryFixture fixture;
  auto                      document = CreateDefaultPipelineDocument();
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.extra"}).empty());
  fixture.ReplaceRootDocument(std::move(document));

  AdjustmentTransferDialogModel model;
  std::string                   error;
  ASSERT_TRUE(model.OpenSource(fixture.graph_, fixture.root_document_, &error)) << error;

  const auto before_shape       = SelectionShape(model);
  const auto before_fingerprint = SelectionFingerprint(model);
  const auto focused_items      = model.items()->rowCount();

  model.FocusNode(QStringLiteral("grade.extra"));
  EXPECT_EQ(model.focused_node_id(), QStringLiteral("grade.extra"));
  EXPECT_TRUE(RowValue(model.nodes(), 1, AdjustmentTransferNodeListModel::FocusedRole).toBool());
  EXPECT_FALSE(RowValue(model.nodes(), 0, AdjustmentTransferNodeListModel::FocusedRole).toBool());
  // Item rows swap to the newly focused node.
  EXPECT_NE(model.items()->rowCount(), 0);
  EXPECT_EQ(SelectionShape(model), before_shape);
  EXPECT_EQ(SelectionFingerprint(model), before_fingerprint);
  EXPECT_TRUE(model.can_copy());
  (void)focused_items;
}

// ============================================================================
// Checked state commands
// ============================================================================

TEST(AdjustmentTransferDialogModelTest, NodeCheckSelectsOrClearsEveryOwnedItem) {
  DialogModelHistoryFixture     fixture;
  AdjustmentTransferDialogModel model;
  std::string                   error;
  ASSERT_TRUE(model.OpenSource(fixture.graph_, fixture.root_document_, &error)) << error;

  const NodeId primary{"grade.primary"};
  model.SetNodeChecked(QStringLiteral("grade.primary"), false);
  EXPECT_EQ(model.NodeCheckStateForTesting(primary), Qt::Unchecked);
  EXPECT_EQ(CheckedItemCount(model.items()), 0);
  EXPECT_EQ(RowValue(model.nodes(), 0, AdjustmentTransferNodeListModel::CheckStateRole).toInt(),
            Qt::Unchecked);
  // The DRT/Post node's items stay checked; bulk state derives from all nodes.
  EXPECT_EQ(model.all_nodes_check_state(), Qt::PartiallyChecked);
  EXPECT_TRUE(model.can_copy());

  model.SetNodeChecked(QStringLiteral("grade.primary"), true);
  EXPECT_EQ(model.NodeCheckStateForTesting(primary), Qt::Checked);
  EXPECT_EQ(CheckedItemCount(model.items()), model.items()->rowCount() - 1)
      << "only the disabled Masks row stays unchecked";
}

TEST(AdjustmentTransferDialogModelTest, NodeStateIsPartialWhenSomeItemsAreSelected) {
  DialogModelHistoryFixture     fixture;
  AdjustmentTransferDialogModel model;
  std::string                   error;
  ASSERT_TRUE(model.OpenSource(fixture.graph_, fixture.root_document_, &error)) << error;

  const int exposure = ItemRowForKey(model.items(), QStringLiteral("adj:grade.primary.exposure"));
  ASSERT_GE(exposure, 0);
  model.SetItemChecked(QStringLiteral("grade.primary"),
                       QStringLiteral("adj:grade.primary.exposure"), false);

  EXPECT_EQ(model.NodeCheckStateForTesting(NodeId{"grade.primary"}), Qt::PartiallyChecked);
  EXPECT_EQ(model.focused_items_check_state(), Qt::PartiallyChecked);
  EXPECT_EQ(model.all_nodes_check_state(), Qt::PartiallyChecked);
  EXPECT_EQ(RowValue(model.nodes(), 0, AdjustmentTransferNodeListModel::CheckStateRole).toInt(),
            Qt::PartiallyChecked);
  EXPECT_FALSE(
      RowValue(model.items(), exposure, AdjustmentTransferItemListModel::CheckedRole).toBool());
}

TEST(AdjustmentTransferDialogModelTest, GlobalSelectAllAndClearUpdateEveryNode) {
  DialogModelHistoryFixture fixture;
  auto                      document = CreateDefaultPipelineDocument();
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.extra"}).empty());
  fixture.ReplaceRootDocument(std::move(document));

  AdjustmentTransferDialogModel model;
  std::string                   error;
  ASSERT_TRUE(model.OpenSource(fixture.graph_, fixture.root_document_, &error)) << error;

  model.ClearAll();
  EXPECT_EQ(model.all_nodes_check_state(), Qt::Unchecked);
  EXPECT_FALSE(model.can_copy());
  for (int row = 0; row < model.nodes()->rowCount(); ++row) {
    EXPECT_EQ(RowValue(model.nodes(), row, AdjustmentTransferNodeListModel::CheckStateRole).toInt(),
              Qt::Unchecked);
  }
  // Focus survived the clear: focus and inclusion stay independent.
  EXPECT_EQ(model.focused_node_id(), QStringLiteral("grade.primary"));

  model.SetAllNodesChecked(true);
  EXPECT_EQ(model.all_nodes_check_state(), Qt::Checked);
  EXPECT_TRUE(model.can_copy());
  for (int row = 0; row < model.nodes()->rowCount(); ++row) {
    EXPECT_EQ(RowValue(model.nodes(), row, AdjustmentTransferNodeListModel::CheckStateRole).toInt(),
              Qt::Checked);
  }
}

TEST(AdjustmentTransferDialogModelTest, FocusedItemSelectAllChangesOnlyFocusedNode) {
  DialogModelHistoryFixture fixture;
  auto                      document = CreateDefaultPipelineDocument();
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.extra"}).empty());
  fixture.ReplaceRootDocument(std::move(document));

  AdjustmentTransferDialogModel model;
  std::string                   error;
  ASSERT_TRUE(model.OpenSource(fixture.graph_, fixture.root_document_, &error)) << error;

  model.ClearAll();
  model.FocusNode(QStringLiteral("grade.extra"));
  model.SetAllFocusedNodeItemsChecked(true);

  EXPECT_EQ(model.NodeCheckStateForTesting(NodeId{"grade.extra"}), Qt::Checked);
  EXPECT_EQ(model.NodeCheckStateForTesting(NodeId{"grade.primary"}), Qt::Unchecked);
  EXPECT_EQ(model.NodeCheckStateForTesting(NodeId{"drt"}), Qt::Unchecked);
  EXPECT_EQ(model.focused_items_check_state(), Qt::Checked);
  EXPECT_EQ(model.all_nodes_check_state(), Qt::PartiallyChecked);
  EXPECT_TRUE(model.can_copy());

  model.ClearFocusedNode();
  EXPECT_EQ(model.NodeCheckStateForTesting(NodeId{"grade.extra"}), Qt::Unchecked);
  EXPECT_EQ(model.all_nodes_check_state(), Qt::Unchecked);
  EXPECT_FALSE(model.can_copy());
}

// ============================================================================
// Selection and package construction
// ============================================================================

TEST(AdjustmentTransferDialogModelTest, BuildSelectionIncludesOnlyCheckedEnabledItems) {
  DialogModelHistoryFixture fixture;
  auto                      document = CreateDefaultPipelineDocument();
  grade_mask_test::AddRadialMask(document, MaskId{"mask.a"});
  fixture.ReplaceRootDocument(std::move(document));

  AdjustmentTransferDialogModel model;
  std::string                   error;
  ASSERT_TRUE(model.OpenSource(fixture.graph_, fixture.root_document_, &error)) << error;

  model.SetItemChecked(QStringLiteral("grade.primary"), QStringLiteral("masks"), false);
  model.SetItemChecked(QStringLiteral("grade.primary"),
                       QStringLiteral("adj:grade.primary.exposure"), false);

  const auto selection = model.BuildSelection();
  ASSERT_EQ(selection.source_version_id, fixture.graph_->GetActiveVersionId());
  ASSERT_EQ(selection.nodes.size(), 2u);
  EXPECT_EQ(selection.nodes.at(0).node_id, NodeId{"grade.primary"});
  EXPECT_EQ(selection.nodes.at(1).node_id, NodeId{"drt"});
  for (const auto& item : selection.nodes.at(0).items) {
    EXPECT_NE(item.kind, AdjustmentTransferItemKind::Masks);
    if (item.kind == AdjustmentTransferItemKind::Adjustment) {
      ASSERT_TRUE(item.adjustment_id.has_value());
      EXPECT_NE(*item.adjustment_id, AdjustmentInstanceId{"grade.primary.exposure"});
    }
  }
}

TEST(AdjustmentTransferDialogModelTest, EmptySelectionFailsPackageBuildClosed) {
  DialogModelHistoryFixture     fixture;
  AdjustmentTransferDialogModel model;
  std::string                   error;
  ASSERT_TRUE(model.OpenSource(fixture.graph_, fixture.root_document_, &error)) << error;

  model.ClearAll();
  EXPECT_FALSE(model.can_copy());
  std::string build_error;
  const auto  package = model.BuildPackage(&build_error);
  EXPECT_FALSE(package.has_value());
  EXPECT_FALSE(build_error.empty());
  // The failed build does not disturb the (empty) selection state.
  EXPECT_EQ(model.all_nodes_check_state(), Qt::Unchecked);
}

// ============================================================================
// Version replay failure keeps prior valid state
// ============================================================================

TEST(AdjustmentTransferDialogModelTest, FailedVersionReplayKeepsPriorSelection) {
  DialogModelHistoryFixture fixture;
  ASSERT_TRUE(fixture.CommitExposure(1.5, 2.0).committed);
  const auto good_id = fixture.graph_->CreateVersionRefAtActiveHead("Good", 10);

  // A commit targeting a node no document contains publishes to the graph but
  // cannot apply during replay.
  const auto broken  = fixture.history_->AppendEdit(
      MakeSetParameterBatch(alcedo::test::ColorGradeFieldTarget("exposure", "grade.ghost"),
                             {{"exposure_ev", 2.0}}, {{"exposure_ev", 3.0}}, true, true, "Ghost"));
  ASSERT_TRUE(broken.committed) << broken.error;
  const auto broken_id = fixture.graph_->CreateVersionRefAtActiveHead("Broken", 20);

  AdjustmentTransferDialogModel model;
  std::string                   error;
  ASSERT_TRUE(model.OpenSource(fixture.graph_, fixture.root_document_, &error)) << error;

  model.SelectVersion(QString::fromStdString(good_id.ToString()));
  ASSERT_EQ(model.selected_version_id(), QString::fromStdString(good_id.ToString()));
  const auto good_shape = SelectionShape(model);

  model.SelectVersion(QString::fromStdString(broken_id.ToString()));
  EXPECT_EQ(model.selected_version_id(), QString::fromStdString(good_id.ToString()))
      << "the prior valid Version stays selected after a failed replay";
  EXPECT_EQ(SelectionShape(model), good_shape);
  EXPECT_FALSE(model.error_text().isEmpty());
  // The model never silently falls back to the active Version ("Default").
  EXPECT_NE(model.selected_version_id(),
            QString::fromStdString(fixture.graph_->GetActiveVersionId().ToString()));
}

TEST(AdjustmentTransferDialogModelTest, InvalidVersionIdentityFailsClosed) {
  DialogModelHistoryFixture     fixture;
  AdjustmentTransferDialogModel model;
  std::string                   error;
  ASSERT_TRUE(model.OpenSource(fixture.graph_, fixture.root_document_, &error)) << error;

  const auto prior_id = model.selected_version_id();
  model.SelectVersion(QStringLiteral("not-a-version-id"));
  EXPECT_EQ(model.selected_version_id(), prior_id);
  EXPECT_FALSE(model.error_text().isEmpty());
}

}  // namespace alcedo::ui::test
