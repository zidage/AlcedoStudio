//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file adjustment_transfer_controller_test.cpp
/// @brief NM10.4 controller/coordinator tests against a real packed project:
///        the controller only routes commands and owns the copied package;
///        source inspection must not mutate the live source pipeline, and
///        the apply coordinator refreshes only successfully persisted targets.

#include "ui/alcedo_main/album_backend/adjustment_transfer_controller.hpp"

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/editor_pipeline_command_service.hpp"
#include "app/pipeline_service.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/commit_types.hpp"
#include "ui/album_backend_seeded_project_fixture.hpp"
#include "ui/album_backend_test_fixture.hpp"
#include "ui/alcedo_main/album_backend/adjustment_transfer_apply_coordinator.hpp"
#include "ui/alcedo_main/album_backend/adjustment_transfer_dialog_model.hpp"
#include "ui/alcedo_main/album_backend/adjustment_transfer_list_models.hpp"
#include "ui/alcedo_main/album_backend/application_module_host.hpp"
#include "ui/alcedo_main/album_backend/library_module.hpp"
#include "ui/alcedo_main/album_backend/project_module.hpp"

namespace alcedo::ui::test {
namespace {

/// Live source-pipeline state that source inspection must never change.
struct SourceGuardSnapshot {
  version_ref_id_t   active_version_id;
  head_commit_hash_t active_head;
  std::size_t        commit_count;
  std::size_t        version_count;
  bool               dirty;
  bool               serialized_writeback;
  std::string        live_document_json;
  std::uint64_t      history_rebuilds;
};

auto CaptureSourceGuard(const std::shared_ptr<PipelineGuard>& guard, PipelineMgmtService* service)
    -> SourceGuardSnapshot {
  SourceGuardSnapshot snapshot{};
  snapshot.active_version_id    = guard->commit_graph_->GetActiveVersionId();
  snapshot.active_head          = guard->commit_graph_->GetActiveVersionRef().head_commit_hash;
  snapshot.commit_count         = guard->commit_graph_->CommitCount();
  snapshot.version_count        = guard->commit_graph_->GetAllVersionRefs().size();
  snapshot.dirty                = guard->dirty_;
  snapshot.serialized_writeback = guard->serialized_state_needs_writeback_;
  snapshot.live_document_json   = CanonicalPipelineDocumentJson(*guard->document_);
  snapshot.history_rebuilds     = service->EditorPipelineHistoryRebuildCount();
  return snapshot;
}

void ExpectSourceGuardUnchanged(const SourceGuardSnapshot& before,
                                const SourceGuardSnapshot& after) {
  EXPECT_EQ(after.active_version_id, before.active_version_id);
  EXPECT_EQ(HeadCommitHashToStorage(after.active_head),
            HeadCommitHashToStorage(before.active_head));
  EXPECT_EQ(after.commit_count, before.commit_count);
  EXPECT_EQ(after.version_count, before.version_count);
  EXPECT_EQ(after.dirty, before.dirty);
  EXPECT_EQ(after.serialized_writeback, before.serialized_writeback);
  EXPECT_EQ(after.live_document_json, before.live_document_json);
  EXPECT_EQ(after.history_rebuilds, before.history_rebuilds);
}

auto ItemRowForKey(const QAbstractItemModel* model, const QString& key) -> int {
  for (int row = 0; row < model->rowCount(); ++row) {
    if (model->data(model->index(row, 0), AdjustmentTransferItemListModel::ItemKeyRole)
            .toString() == key) {
      return row;
    }
  }
  return -1;
}

class AdjustmentTransferControllerTest : public ApplicationModuleHostTestFixture {
 protected:
  auto LoadSeededBackend(ApplicationModuleHost& backend, std::size_t image_count = 2)
      -> std::optional<SeededProject> {
    auto seeded = CreateSeededPackedProject(temp_dir_, {}, image_count);
    if (!seeded.has_value()) {
      return std::nullopt;
    }
    if (!LoadPackedProject(backend, seeded->packed_path_)) {
      return std::nullopt;
    }
    return seeded;
  }
};

// ============================================================================
// Copy: the controller prepares the dialog model and publishes the package;
// the live source pipeline is never mutated in the process.
// ============================================================================

TEST_F(AdjustmentTransferControllerTest, CopyDoesNotSaveOrRenderSourceImage) {
  ApplicationModuleHost backend;
  auto                  seeded = LoadSeededBackend(backend, 2);
  ASSERT_TRUE(seeded.has_value());
  ASSERT_GE(seeded->images_.size(), 2u);
  const auto source   = seeded->images_.front();

  auto*      transfer = backend.adjustment_transfer();
  ASSERT_NE(transfer, nullptr);

  const auto prepare = transfer->PrepareCopy(static_cast<uint>(source.file_id_));
  ASSERT_TRUE(prepare.value("success").toBool())
      << prepare.value("message").toString().toStdString();
  EXPECT_EQ(prepare.value("sourceTitle").toString(), QStringLiteral("album-delete-0.dng"));
  EXPECT_FALSE(prepare.value("activeVersionId").toString().isEmpty());
  // The routing result carries provenance only; rows live on the dialog model.
  EXPECT_FALSE(prepare.contains("adjustmentRows"));
  EXPECT_FALSE(prepare.contains("selectedKeys"));

  auto pipeline_service = backend.project()->handler().pipeline_service();
  ASSERT_TRUE(pipeline_service);
  const auto guard = pipeline_service->LoadEditorPipeline(source.file_id_);
  ASSERT_TRUE(guard && guard->commit_graph_ && guard->document_);
  const auto before = CaptureSourceGuard(guard, pipeline_service.get());

  // A model command (focus + uncheck) and the Copy build must not touch the
  // live source graph, document, dirty flag, WAL, or render counters.
  auto*      model  = transfer->dialog_model();
  ASSERT_NE(model, nullptr);
  model->FocusNode(QStringLiteral("drt"));
  model->SetAllFocusedNodeItemsChecked(false);
  model->FocusNode(QStringLiteral("grade.primary"));

  const auto copy = transfer->CommitCopy();
  ASSERT_TRUE(copy.value("success").toBool()) << copy.value("message").toString().toStdString();
  EXPECT_TRUE(transfer->package_available());
  EXPECT_EQ(transfer->package_source_title(), QStringLiteral("album-delete-0.dng"));
  EXPECT_FALSE(transfer->package_source_version().isEmpty());
  EXPECT_FALSE(transfer->package_summary().isEmpty());

  const auto after = CaptureSourceGuard(guard, pipeline_service.get());
  ExpectSourceGuardUnchanged(before, after);
}

TEST_F(AdjustmentTransferControllerTest, CopyFailureKeepsPriorPackage) {
  ApplicationModuleHost backend;
  auto                  seeded = LoadSeededBackend(backend, 1);
  ASSERT_TRUE(seeded.has_value());
  const auto source   = seeded->images_.front();

  auto*      transfer = backend.adjustment_transfer();
  ASSERT_TRUE(transfer->PrepareCopy(static_cast<uint>(source.file_id_)).value("success").toBool());
  ASSERT_TRUE(transfer->CommitCopy().value("success").toBool());
  ASSERT_TRUE(transfer->package_available());
  const auto summary        = transfer->package_summary();
  const auto source_title   = transfer->package_source_title();
  const auto source_version = transfer->package_source_version();

  // A package build that fails validation must leave the published package,
  // its summary, and Paste availability untouched.
  auto*      model          = transfer->dialog_model();
  model->ClearAll();
  ASSERT_FALSE(model->can_copy());
  const auto failed = transfer->CommitCopy();
  EXPECT_FALSE(failed.value("success").toBool());
  EXPECT_FALSE(failed.value("message").toString().isEmpty());
  EXPECT_TRUE(transfer->package_available());
  EXPECT_EQ(transfer->package_summary(), summary);
  EXPECT_EQ(transfer->package_source_title(), source_title);
  EXPECT_EQ(transfer->package_source_version(), source_version);
}

TEST_F(AdjustmentTransferControllerTest, ControllerNoLongerOwnsTransferRowFormatting) {
  ApplicationModuleHost backend;
  auto                  seeded = LoadSeededBackend(backend, 1);
  ASSERT_TRUE(seeded.has_value());
  const auto source   = seeded->images_.front();

  auto*      transfer = backend.adjustment_transfer();
  ASSERT_TRUE(transfer->PrepareCopy(static_cast<uint>(source.file_id_)).value("success").toBool());

  // Rows come from the catalog-driven dialog model, keyed by stable identity —
  // not from a controller-owned spec array.
  auto* items = transfer->dialog_model()->items();
  ASSERT_NE(items, nullptr);
  const int exposure = ItemRowForKey(items, QStringLiteral("adj:grade.primary.exposure"));
  ASSERT_GE(exposure, 0);
  EXPECT_EQ(items->data(items->index(exposure, 0), AdjustmentTransferItemListModel::DisplayNameRole)
                .toString(),
            QStringLiteral("Exposure"));
  EXPECT_TRUE(items->data(items->index(exposure, 0), AdjustmentTransferItemListModel::CheckedRole)
                  .toBool());
}

// ============================================================================
// Paste: the coordinator owns per-target apply + refresh; a failed target is
// restored and never refreshed.
// ============================================================================

TEST_F(AdjustmentTransferControllerTest, MultiTargetCoordinatorRefreshesOnlySuccessfulTargets) {
  ApplicationModuleHost backend;
  auto                  seeded = LoadSeededBackend(backend, 2);
  ASSERT_TRUE(seeded.has_value());
  const auto source   = seeded->images_.at(0);
  const auto target   = seeded->images_.at(1);

  auto*      transfer = backend.adjustment_transfer();
  ASSERT_TRUE(transfer->PrepareCopy(static_cast<uint>(source.file_id_)).value("success").toBool());
  ASSERT_TRUE(transfer->CommitCopy().value("success").toBool());

  auto* coordinator = transfer->apply_coordinator();
  ASSERT_NE(coordinator, nullptr);
  QSignalSpy refreshed_spy(coordinator, &AdjustmentTransferApplyCoordinator::TargetRefreshed);

  const QVariantList targets{
      QVariantMap{{"elementId", static_cast<uint>(target.file_id_)},
                  {"imageId", static_cast<uint>(target.image_id_)}},
  };
  const auto paste = transfer->Paste(targets, QStringLiteral("paste"));
  EXPECT_EQ(paste.value("appliedCount").toInt(), 1);
  EXPECT_EQ(paste.value("failureCount").toInt(), 0);
  ASSERT_EQ(refreshed_spy.size(), 1);
  EXPECT_EQ(refreshed_spy.front().front().toUInt(), static_cast<uint>(target.file_id_));

  // A package the planner rejects: empty sparse v6 carries nothing
  // transferable, so the apply fails before any graph mutation.
  alcedo::AdjustmentTransferPackage empty_package;
  ASSERT_TRUE(empty_package.Empty());
  const auto failed =
      coordinator->ApplyToTargets(empty_package, {target.file_id_}, QStringLiteral("fail-probe"));
  EXPECT_EQ(failed.value("appliedCount").toInt(), 0);
  EXPECT_EQ(failed.value("failureCount").toInt(), 1);
  const auto failures = failed.value("failures").toList();
  ASSERT_EQ(failures.size(), 1);
  EXPECT_FALSE(failures.front().toMap().value("message").toString().isEmpty());
  // The failed target received no HDR write, no thumbnail work, and no
  // TargetRefreshed emission — the spy still holds only the first success.
  EXPECT_EQ(refreshed_spy.size(), 1);

  // The pasted target gained a root-relative Version it did not have before.
  auto       pipeline_service = backend.project()->handler().pipeline_service();
  const auto target_guard     = pipeline_service->LoadEditorPipeline(target.file_id_);
  ASSERT_TRUE(target_guard && target_guard->commit_graph_);
  bool found_pasted_version = false;
  for (const auto& kv : target_guard->commit_graph_->GetAllVersionRefs()) {
    if (kv.second.display_name == "Pasted Adjustments") {
      found_pasted_version = true;
      break;
    }
  }
  EXPECT_TRUE(found_pasted_version);
}

}  // namespace
}  // namespace alcedo::ui::test
