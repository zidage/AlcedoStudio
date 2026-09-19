//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/adjustment_transfer_controller.hpp"

#include <memory>
#include <unordered_set>
#include <vector>

#include "app/pipeline_service.hpp"
#include "ui/alcedo_main/album_backend/album_types.hpp"
#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"
#include "ui/alcedo_main/album_backend/import_export.hpp"
#include "ui/alcedo_main/album_backend/library_module.hpp"
#include "ui/alcedo_main/album_backend/project_module.hpp"
#include "ui/alcedo_main/i18n.hpp"

namespace alcedo::ui {
namespace {

auto ErrorResult(const QString& message) -> QVariantMap {
  return {{"success", false}, {"message", message}};
}

auto SuccessResult(const QString& message = {}) -> QVariantMap {
  QVariantMap result{{"success", true}};
  if (!message.isEmpty()) {
    result.insert("message", message);
  }
  return result;
}

auto SessionResultMap(const alcedo::EditorSessionResult& result) -> QVariantMap {
  const bool success = result.kind != alcedo::EditorSessionResultKind::Rejected &&
                       result.kind != alcedo::EditorSessionResultKind::Failed;
  return {{"success", success},
          {"message", QString::fromStdString(result.message)},
          {"kind", static_cast<int>(result.kind)}};
}

auto CurrentExceptionText(const char* fallback) -> QString {
  try {
    throw;
  } catch (const std::exception& e) {
    return QString::fromUtf8(e.what());
  } catch (...) {
    return QString::fromUtf8(fallback);
  }
}

auto TitleForItem(const AlbumItem* item, sl_element_id_t element_id) -> QString {
  if (item != nullptr && !item->file_name.isEmpty()) {
    return item->file_name;
  }
  return Tr("Image") + QStringLiteral(" ") + QString::number(static_cast<qulonglong>(element_id));
}

auto MakeTargetIds(const std::vector<ExportTarget>& targets) -> std::vector<sl_element_id_t> {
  std::vector<sl_element_id_t>        ids;
  std::unordered_set<sl_element_id_t> seen;
  ids.reserve(targets.size());
  seen.reserve(targets.size() * 2 + 1);
  for (const auto& [element_id, image_id] : targets) {
    (void)image_id;
    if (element_id == 0 || !seen.insert(element_id).second) {
      continue;
    }
    ids.push_back(element_id);
  }
  return ids;
}

}  // namespace

AdjustmentTransferController::AdjustmentTransferController(ProjectModule*       project,
                                                           LibraryModule*       library,
                                                           ImportExportHandler* import_export,
                                                           QObject*             parent)
    : QObject(parent),
      project_(project),
      library_(library),
      import_export_(import_export),
      dialog_model_(new AdjustmentTransferDialogModel(this)),
      apply_coordinator_(
          std::make_unique<AdjustmentTransferApplyCoordinator>(project, library, this)) {}

auto AdjustmentTransferController::PrepareCopy(uint elementId) -> QVariantMap {
  auto pipeline_service = project_->handler().pipeline_service();
  if (!pipeline_service) {
    return ErrorResult(Tr("Pipeline service is unavailable."));
  }
  if (elementId == 0) {
    return ErrorResult(Tr("No image selected."));
  }

  try {
    const auto guard =
        pipeline_service->LoadEditorPipeline(static_cast<sl_element_id_t>(elementId));
    if (!guard || !guard->commit_graph_ || !guard->root_document_) {
      return ErrorResult(Tr("Pipeline was not available."));
    }

    std::string error;
    if (!dialog_model_->OpenSource(guard->commit_graph_, guard->root_document_, &error)) {
      return ErrorResult(QString::fromStdString(error));
    }

    const auto* item       = library_->FindAlbumItem(static_cast<sl_element_id_t>(elementId));
    prepared_source_title_ = TitleForItem(item, static_cast<sl_element_id_t>(elementId));

    QVariantMap result     = SuccessResult();
    result.insert("sourceTitle", prepared_source_title_);
    result.insert("activeVersionId", dialog_model_->selected_version_id());
    return result;
  } catch (...) {
    return ErrorResult(CurrentExceptionText("Failed to prepare adjustment copy."));
  }
}

auto AdjustmentTransferController::CommitCopy() -> QVariantMap {
  std::string error;
  auto        package = dialog_model_->BuildPackage(&error);
  if (!package.has_value()) {
    return ErrorResult(error.empty() ? Tr("No transferable adjustments.")
                                     : QString::fromStdString(error));
  }

  copied_package_        = std::move(*package);
  copied_summary_        = dialog_model_->SelectionSummary();
  copied_source_title_   = prepared_source_title_;
  copied_source_version_ = dialog_model_->selected_version_name();
  emit        PackageChanged();

  QVariantMap result = SuccessResult(Tr("Adjustments copied."));
  result.insert("count", static_cast<int>(copied_summary_.size()));
  result.insert("summary", copied_summary_);
  return result;
}

auto AdjustmentTransferController::Paste(const QVariantList& targetEntries, const QString& strategy)
    -> QVariantMap {
  if (!copied_package_.has_value()) {
    return ErrorResult(Tr("No copied adjustments."));
  }
  const bool merge_strategy = strategy != QStringLiteral("paste");
  if (merge_strategy) {
    return ErrorResult(Tr("Pipeline merge is not supported."));
  }

  const auto targets = import_export_->CollectExportTargets(targetEntries);
  const auto ids     = MakeTargetIds(targets);
  if (ids.empty()) {
    return ErrorResult(Tr("No target images selected."));
  }
  return apply_coordinator_->ApplyToTargets(*copied_package_, ids, Tr("Pasted Adjustments"));
}

auto AdjustmentTransferController::PasteIntoEditor(QObject* editorSession) -> QVariantMap {
  if (!copied_package_.has_value()) return ErrorResult(Tr("No copied adjustments."));
  auto* session = qobject_cast<EditorSessionController*>(editorSession);
  if (!session) return ErrorResult(Tr("Editor session is unavailable."));
  return SessionResultMap(
      session->PasteAdjustmentPackage(*copied_package_, Tr("Pasted Adjustments")));
}

void AdjustmentTransferController::Discard() {
  copied_package_.reset();
  copied_summary_.clear();
  copied_source_title_.clear();
  copied_source_version_.clear();
  emit PackageChanged();
}

}  // namespace alcedo::ui
