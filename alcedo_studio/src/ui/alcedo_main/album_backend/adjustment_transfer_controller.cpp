//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/adjustment_transfer_controller.hpp"

#include <algorithm>
#include <memory>
#include <unordered_set>
#include <vector>

#include "app/pipeline_service.hpp"
#include "edit/history/commit_graph.hpp"
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

AdjustmentTransferController::AdjustmentTransferController(
    ProjectModule* project, LibraryModule* library, ImportExportHandler* import_export,
    BackgroundTaskController* background_tasks, QObject* parent)
    : QObject(parent),
      project_(project),
      library_(library),
      import_export_(import_export),
      dialog_model_(new AdjustmentTransferDialogModel(this)),
      apply_coordinator_(std::make_unique<AdjustmentTransferApplyCoordinator>(
          project, library, background_tasks, this)) {
  connect(apply_coordinator_.get(), &AdjustmentTransferApplyCoordinator::RunningChanged, this,
          &AdjustmentTransferController::PasteInProgressChanged);
  connect(apply_coordinator_.get(), &AdjustmentTransferApplyCoordinator::ApplyFinished, this,
          &AdjustmentTransferController::PasteFinished);
}

void AdjustmentTransferController::SetEditorSession(EditorSessionController* editor_session) {
  editor_session_ = editor_session;
}

auto AdjustmentTransferController::PrepareCopy(uint elementId) -> QVariantMap {
  auto pipeline_service = project_->handler().pipeline_service();
  if (!pipeline_service) {
    return ErrorResult(Tr("Pipeline service is unavailable."));
  }
  if (elementId == 0) {
    return ErrorResult(Tr("No image selected."));
  }

  try {
    std::shared_ptr<const alcedo::CommitGraph>      source_graph;
    std::shared_ptr<const alcedo::PipelineDocument> source_root;
    std::string                                     error;
    // The editor session owns the history of the image it has open. Read that
    // image through the session; loading it here would rebind its history and
    // live document from storage and drop the unsaved edits.
    if (editor_session_ && editor_session_->has_image() &&
        editor_session_->element_id() == elementId) {
      if (!editor_session_->SnapshotHistorySource(&source_graph, &source_root, &error)) {
        return ErrorResult(error.empty() ? Tr("Pipeline was not available.")
                                         : QString::fromStdString(error));
      }
    } else {
      const auto guard =
          pipeline_service->LoadEditorPipeline(static_cast<sl_element_id_t>(elementId));
      if (!guard || !guard->commit_graph_ || !guard->root_document_) {
        pipeline_service->ReleasePipelineUse(guard);
        return ErrorResult(Tr("Pipeline was not available."));
      }
      // Detach from the cached guard so a later Paste into this image cannot
      // mutate the graph the dialog is reading.
      source_graph = std::make_shared<const alcedo::CommitGraph>(*guard->commit_graph_);
      source_root  = guard->root_document_;
      pipeline_service->ReleasePipelineUse(guard);
    }

    const bool opened = dialog_model_->OpenSource(source_graph, source_root, &error);
    if (!opened) {
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
  auto       ids     = MakeTargetIds(targets);
  if (ids.empty()) {
    return ErrorResult(Tr("No target images selected."));
  }

  // The editor session owns the history of the image it has open. Loading that
  // pipeline here would change its graph behind the session, so its Versions
  // panel would miss the new Version. Paste it through the session queue.
  QString editor_message;
  bool    editor_pasted = false;
  if (editor_session_ && editor_session_->has_image()) {
    const auto editor_id = static_cast<sl_element_id_t>(editor_session_->element_id());
    const auto found     = std::find(ids.begin(), ids.end(), editor_id);
    if (found != ids.end()) {
      ids.erase(found);
      const auto session_result = SessionResultMap(
          editor_session_->PasteAdjustmentPackage(*copied_package_, Tr("Pasted Adjustments")));
      editor_pasted  = session_result.value("success").toBool();
      editor_message = session_result.value("message").toString();
      if (!editor_pasted && ids.empty()) {
        return ErrorResult(editor_message.isEmpty() ? Tr("Editor Paste failed.") : editor_message);
      }
    }
  }
  if (ids.empty()) {
    QVariantMap result = SuccessResult(Tr("Adjustments pasted."));
    result.insert("pending", false);
    return result;
  }

  QString error;
  if (!apply_coordinator_->StartApplyToTargets(*copied_package_, std::move(ids),
                                               Tr("Pasted Adjustments"), &error)) {
    return ErrorResult(error);
  }
  QVariantMap result = SuccessResult(Tr("Pasting adjustments…"));
  result.insert("pending", true);
  if (!editor_pasted && !editor_message.isEmpty()) {
    result.insert("editorMessage", editor_message);
  }
  return result;
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
