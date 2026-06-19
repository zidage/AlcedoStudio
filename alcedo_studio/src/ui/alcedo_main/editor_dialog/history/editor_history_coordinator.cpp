//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/editor_dialog/history/editor_history_coordinator.hpp"

#include <utility>

#include <QInputDialog>
#include <QListWidgetItem>
#include <QMessageBox>

#include "ui/alcedo_main/editor_dialog/controllers/history_controller.hpp"
#include "ui/alcedo_main/i18n.hpp"
#include "utils/diagnostics/app_logging.hpp"

namespace alcedo::ui {

EditorHistoryCoordinator::EditorHistoryCoordinator(Dependencies dependencies, Callbacks callbacks)
    : dependencies_(std::move(dependencies)), callbacks_(std::move(callbacks)) {}

auto EditorHistoryCoordinator::WorkingVersion() -> alcedo::WorkingVersion& { return working_version_; }

auto EditorHistoryCoordinator::WorkingVersion() const -> const alcedo::WorkingVersion& {
  return working_version_;
}

void EditorHistoryCoordinator::SetUiContext(const versioning::VersionUiContext& ui) {
  ui_ = ui;
  ui_callbacks_.request_rename_version =
      [this](const QString& version_id) { RenameVersionById(version_id); };
}

void EditorHistoryCoordinator::SeedWorkingVersionFromActive() {
  if (dependencies_.history_guard && dependencies_.history_guard->history_ &&
      dependencies_.pipeline_guard && dependencies_.pipeline_guard->pipeline_) {
    auto& history         = *dependencies_.history_guard->history_;
    auto& default_version = history.GetDefaultVersion();
    if (history.GetVersions().size() == 1 && default_version.GetAllEditTransactions().empty()) {
      history.SetImportPipelineParams(dependencies_.pipeline_guard->pipeline_->ExportPipelineParams());
      dependencies_.history_guard->dirty_ = true;
    }

    const auto active_params =
        history.ReconstructPipelineParamsForVersion(history.GetActiveVersionID());
    if (active_params.has_value() &&
        dependencies_.pipeline_guard->pipeline_->ExportPipelineParams() != *active_params) {
      dependencies_.pipeline_guard->pipeline_->ImportPipelineParams(*active_params);
      dependencies_.pipeline_guard->dirty_ = true;
      if (callbacks_.after_pipeline_params_imported) {
        callbacks_.after_pipeline_params_imported();
      }
    }
  }
  working_version_ =
      controllers::SeedWorkingVersionFromActive(dependencies_.element_id,
                                                dependencies_.history_guard);
}

auto EditorHistoryCoordinator::ReconstructPipelineParamsForVersion(Version& version) const
    -> std::optional<nlohmann::json> {
  return versioning::ReconstructPipelineParamsForVersion(version, dependencies_.history_guard);
}

auto EditorHistoryCoordinator::ReloadUiStateFromPipeline(bool reset_to_defaults_if_missing)
    -> bool {
  diag::TraceScope trace(diag::editorLog(), QStringLiteral("editor.history.reload_ui_state"),
                         QStringLiteral("reset_defaults=%1")
                             .arg(reset_to_defaults_if_missing ? QStringLiteral("true")
                                                               : QStringLiteral("false")));
  return callbacks_.reload_ui_state_from_pipeline
             ? callbacks_.reload_ui_state_from_pipeline(reset_to_defaults_if_missing)
             : false;
}

auto EditorHistoryCoordinator::ApplyPipelineParamsToEditor(const nlohmann::json& params) -> bool {
  diag::TraceScope trace(diag::editorLog(), QStringLiteral("editor.history.apply_pipeline_params"),
                         QStringLiteral("json_bytes=%1")
                             .arg(static_cast<qulonglong>(params.dump().size())));
  if (!dependencies_.pipeline_guard || !dependencies_.pipeline_guard->pipeline_) {
    return false;
  }

  auto exec = dependencies_.pipeline_guard->pipeline_;
  exec->ImportPipelineParams(params);
  dependencies_.pipeline_guard->dirty_ = true;
  if (callbacks_.after_pipeline_params_imported) {
    callbacks_.after_pipeline_params_imported();
  }

  return ReloadUiStateFromPipeline(/*reset_to_defaults_if_missing=*/true);
}

auto EditorHistoryCoordinator::ReloadEditorFromHistoryVersion(Version& version, QString* error)
    -> bool {
  diag::TraceScope trace(diag::editorLog(), QStringLiteral("editor.history.reload_version"),
                         QStringLiteral("version_id=%1")
                             .arg(QString::fromStdString(version.GetVersionID().ToString())));
  const auto selected_params = ReconstructPipelineParamsForVersion(version);
  if (!selected_params.has_value()) {
    if (error) {
      *error = Tr("Could not reconstruct pipeline params for the selected version.");
    }
    return false;
  }

  if (!ApplyPipelineParamsToEditor(*selected_params)) {
    if (error) {
      *error = Tr("Failed to apply selected version to the editor.");
    }
    return false;
  }
  return true;
}

void EditorHistoryCoordinator::CheckoutSelectedVersion(QListWidgetItem* item) {
  if (!item) {
    return;
  }
  CheckoutVersionById(item->data(Qt::UserRole).toString());
}

void EditorHistoryCoordinator::CheckoutVersionById(const QString& version_id) {
  diag::TraceScope trace(diag::editorLog(), QStringLiteral("editor.history.checkout"),
                         QStringLiteral("version_id=%1").arg(version_id));
  versioning::ResolvedVersionSelection selection{};
  QString                              selection_error;
  if (!versioning::ResolveVersionId(version_id, dependencies_.history_guard, &selection,
                                    &selection_error)) {
    if (!selection_error.isEmpty()) {
      QMessageBox::warning(dependencies_.message_parent, Tr("History"), selection_error);
    }
    return;
  }

  QString reload_error;
  if (!selection.version || !ReloadEditorFromHistoryVersion(*selection.version, &reload_error)) {
    QMessageBox::warning(dependencies_.message_parent, Tr("History"), reload_error);
    return;
  }

  if (dependencies_.history_service) {
    dependencies_.history_service->SetActiveVersion(dependencies_.history_guard,
                                                    selection.version_id);
  }
  working_version_ =
      versioning::SeedWorkingVersionFromVersion(dependencies_.element_id, selection.version_id,
                                                dependencies_.history_guard);
  UpdateVersionUi();
}

void EditorHistoryCoordinator::RenameVersionById(const QString& version_id) {
  versioning::ResolvedVersionSelection selection{};
  QString                              selection_error;
  if (!versioning::ResolveVersionId(version_id, dependencies_.history_guard, &selection,
                                    &selection_error) ||
      !selection.version) {
    if (!selection_error.isEmpty()) {
      QMessageBox::warning(dependencies_.message_parent, Tr("Versions"), selection_error);
    }
    return;
  }

  const QString current_name = QString::fromStdString(selection.version->GetDisplayName());
  bool          ok           = false;
  const QString next_name    = QInputDialog::getText(
      dependencies_.message_parent, Tr("Rename version"), Tr("Version name"),
      QLineEdit::Normal, current_name, &ok);
  if (!ok) {
    return;
  }
  const QString trimmed = next_name.trimmed();
  if (trimmed.isEmpty() || trimmed == current_name) {
    return;
  }
  dependencies_.history_service->RenameVersion(dependencies_.history_guard, selection.version_id,
                                                trimmed.toStdString());
  UpdateVersionUi();
}

void EditorHistoryCoordinator::UndoLastTransaction() {
  diag::TraceScope trace(diag::editorLog(), QStringLiteral("editor.history.undo"),
                         QStringLiteral("element_id=%1")
                             .arg(static_cast<qulonglong>(dependencies_.element_id)));
  if (!dependencies_.pipeline_guard || !dependencies_.pipeline_guard->pipeline_) {
    return;
  }

  const auto undo_result =
      versioning::UndoLastTransaction(working_version_, dependencies_.pipeline_guard);
  if (!undo_result.moved && undo_result.error.isEmpty()) {
    QMessageBox::information(dependencies_.message_parent, Tr("History"),
                             Tr("No transaction to undo."));
    return;
  }
  if (!undo_result.error.isEmpty()) {
    QMessageBox::warning(dependencies_.message_parent, Tr("History"), undo_result.error);
    return;
  }
  if (!ReloadUiStateFromPipeline(/*reset_to_defaults_if_missing=*/false)) {
    QMessageBox::warning(dependencies_.message_parent, Tr("History"),
                         Tr("Undo failed while reloading pipeline state."));
    return;
  }
  versioning::PersistWorkingVersion(dependencies_.history_service, dependencies_.history_guard,
                                    working_version_, dependencies_.pipeline_guard);
  UpdateVersionUi();
}

void EditorHistoryCoordinator::MoveCursorTo(size_t target_cursor) {
  diag::TraceScope trace(diag::editorLog(), QStringLiteral("editor.history.move_cursor"),
                         QStringLiteral("target_cursor=%1")
                             .arg(static_cast<qulonglong>(target_cursor)));
  const auto move_result =
      versioning::MoveCursorTo(working_version_, target_cursor, dependencies_.pipeline_guard);
  if (!move_result.error.isEmpty()) {
    QMessageBox::warning(dependencies_.message_parent, Tr("History"), move_result.error);
    return;
  }
  if (!move_result.moved) {
    return;
  }
  if (!ReloadUiStateFromPipeline(/*reset_to_defaults_if_missing=*/false)) {
    QMessageBox::warning(dependencies_.message_parent, Tr("History"),
                         Tr("History jump failed while reloading pipeline state."));
    return;
  }
  versioning::PersistWorkingVersion(dependencies_.history_service, dependencies_.history_guard,
                                    working_version_, dependencies_.pipeline_guard);
  UpdateVersionUi();
}

void EditorHistoryCoordinator::UpdateVersionUi() {
  diag::TraceScope trace(diag::editorLog(), QStringLiteral("editor.history.update_version_ui"),
                         QStringLiteral("element_id=%1")
                             .arg(static_cast<qulonglong>(dependencies_.element_id)));
  versioning::PersistWorkingVersion(dependencies_.history_service, dependencies_.history_guard,
                                    working_version_, dependencies_.pipeline_guard);
  versioning::UpdateVersionUi(ui_, ui_callbacks_, working_version_, dependencies_.history_guard,
                              callbacks_.refresh_version_log_selection_styles);
}

void EditorHistoryCoordinator::CreateVersion() {
  diag::TraceScope trace(diag::editorLog(), QStringLiteral("editor.history.create_version"),
                         QStringLiteral("element_id=%1")
                             .arg(static_cast<qulonglong>(dependencies_.element_id)));
  if (!dependencies_.history_service || !dependencies_.history_guard ||
      !dependencies_.history_guard->history_) {
    return;
  }
  const auto version_id = dependencies_.history_service->CreateVersion(dependencies_.history_guard);
  auto&      version    = dependencies_.history_guard->history_->GetVersion(version_id);
  const auto params     = ReconstructPipelineParamsForVersion(version);
  if (params.has_value()) {
    ApplyPipelineParamsToEditor(*params);
  }
  working_version_ = versioning::SeedWorkingVersionFromVersion(
      dependencies_.element_id, version_id, dependencies_.history_guard);
  UpdateVersionUi();
}

}  // namespace alcedo::ui
