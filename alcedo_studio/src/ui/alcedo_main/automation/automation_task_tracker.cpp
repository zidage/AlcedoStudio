//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/automation/automation_task_tracker.hpp"

#include <QVariantList>
#include <QVariantMap>
#include <algorithm>

#include "ui/alcedo_main/album_backend/adjustment_transfer_apply_coordinator.hpp"
#include "ui/alcedo_main/album_backend/adjustment_transfer_controller.hpp"

namespace alcedo::automation {
namespace {

auto BackgroundTaskObject(const QVariantMap& row) -> QJsonObject {
  return QJsonObject{{"task_id", row.value(QStringLiteral("id")).toString()},
                     {"kind", row.value(QStringLiteral("kind")).toString()},
                     {"state", row.value(QStringLiteral("state")).toString()},
                     {"progress", row.value(QStringLiteral("progressPercent")).toInt()},
                     {"title", row.value(QStringLiteral("title")).toString()},
                     {"message", row.value(QStringLiteral("detail")).toString()}};
}

auto ImportCounts(const ui::ImportExportHandler& import_export) -> QJsonObject {
  return QJsonObject{{"total", import_export.ImportTotal()},
                     {"completed", import_export.ImportCompleted()},
                     {"failed", import_export.ImportFailed()},
                     {"unsupported", import_export.ImportUnsupported()},
                     {"excluded", import_export.ImportExcluded()}};
}

auto ExportCounts(const ui::ImportExportHandler& import_export) -> QJsonObject {
  return QJsonObject{{"total", import_export.ExportTotal()},
                     {"completed", import_export.ExportCompleted()},
                     {"succeeded", import_export.ExportSucceeded()},
                     {"failed", import_export.ExportFailed()},
                     {"skipped", import_export.ExportSkipped()}};
}

/// The `result` object of a task of kind `adjustmentPaste` from the coordinator result map.
auto PasteResultObject(const QVariantMap& result) -> QJsonObject {
  QJsonArray applied_ids;
  for (const QVariant& id : result.value(QStringLiteral("appliedIds")).toList()) {
    applied_ids.push_back(static_cast<double>(id.toUInt()));
  }
  QJsonArray failures;
  for (const QVariant& value : result.value(QStringLiteral("failures")).toList()) {
    const QVariantMap failure = value.toMap();
    failures.push_back(QJsonObject{
        {"element_id", static_cast<double>(failure.value(QStringLiteral("elementId")).toUInt())},
        {"message", failure.value(QStringLiteral("message")).toString()}});
  }
  return QJsonObject{{"applied_count", result.value(QStringLiteral("appliedCount")).toInt()},
                     {"failure_count", result.value(QStringLiteral("failureCount")).toInt()},
                     {"applied_ids", applied_ids},
                     {"failures", failures}};
}

/// The status text and, when photos failed, the error summary of the export.
auto ExportMessage(const ui::ImportExportHandler& import_export) -> QString {
  QString message = import_export.ExportStatus();
  if (const QString summary = import_export.ExportErrorSummary(); !summary.isEmpty()) {
    message += QLatin1Char('\n') + summary;
  }
  return message;
}

}  // namespace

AutomationTaskTracker::AutomationTaskTracker(ui::ApplicationModuleHost* host, QObject* parent)
    : QObject(parent), host_(host) {
  if (host_ == nullptr) {
    return;
  }
  connect(host_->import_export(), &ui::ImportExportHandler::ImportStateChanged, this,
          &AutomationTaskTracker::OnImportStateChanged);
  connect(host_->import_export(), &ui::ImportExportHandler::ExportStateChanged, this,
          &AutomationTaskTracker::OnExportStateChanged);
  connect(host_->import_export(), &ui::ImportExportHandler::ExportFinished, this,
          &AutomationTaskTracker::OnExportFinished);
  connect(host_->background_tasks(), &ui::BackgroundTaskController::TasksChanged, this,
          &AutomationTaskTracker::OnBackgroundTasksChanged);
  OnImportStateChanged();
  OnBackgroundTasksChanged();
}

auto AutomationTaskTracker::IsTerminalState(const QString& state) -> bool {
  return state == QStringLiteral("succeeded") || state == QStringLiteral("failed") ||
         state == QStringLiteral("canceled");
}

auto AutomationTaskTracker::NextImportId() -> QString {
  return QStringLiteral("import-%1").arg(++import_count_);
}

auto AutomationTaskTracker::RecordEmptyImport() -> QString {
  const QString id = NextImportId();
  QJsonObject   task{
        {"task_id", id},
        {"kind", "import"},
        {"state", "succeeded"},
        {"progress", 100},
        {"message", ""},
        {"counts",
         QJsonObject{
             {"total", 0}, {"completed", 0}, {"failed", 0}, {"unsupported", 0}, {"excluded", 0}}}};
  finished_tasks_[id] = task;
  emit TaskFinished(task);
  return id;
}

auto AutomationTaskTracker::RunningImportTask() const -> QJsonObject {
  const ui::ImportExportHandler& import_export = *host_->import_export();
  const int                      total         = import_export.ImportTotal();
  const int processed = import_export.ImportCompleted() + import_export.ImportFailed();
  return QJsonObject{{"task_id", running_import_id_},
                     {"kind", "import"},
                     {"state", "running"},
                     {"phase", import_export.ImportPhase()},
                     {"progress", total > 0 ? std::min(100, processed * 100 / total) : -1},
                     {"message", import_export.ImportStatus()},
                     {"counts", ImportCounts(import_export)}};
}

void AutomationTaskTracker::OnImportStateChanged() {
  if (host_ == nullptr) {
    return;
  }
  const ui::ImportExportHandler& import_export = *host_->import_export();
  if (import_export.ImportRunning()) {
    if (running_import_id_.isEmpty()) {
      running_import_id_ = NextImportId();
    }
    emit TaskUpdated(RunningImportTask());
    return;
  }
  if (running_import_id_.isEmpty()) {
    return;
  }
  // The import ended: keep its final counts for its id.
  QJsonObject task{{"task_id", running_import_id_},
                   {"kind", "import"},
                   {"state", "succeeded"},
                   {"progress", 100},
                   {"message", import_export.ImportStatus()},
                   {"counts", ImportCounts(import_export)}};
  finished_tasks_[running_import_id_] = task;
  running_import_id_.clear();
  emit TaskFinished(task);
}

auto AutomationTaskTracker::RunningExportTask() const -> QJsonObject {
  const ui::ImportExportHandler& import_export = *host_->import_export();
  const int                      total         = import_export.ExportTotal();
  return QJsonObject{
      {"task_id", running_export_id_},
      {"kind", "export"},
      {"state", "running"},
      {"progress", total > 0 ? std::min(100, import_export.ExportCompleted() * 100 / total) : -1},
      {"message", import_export.ExportStatus()},
      {"counts", ExportCounts(import_export)}};
}

void AutomationTaskTracker::OnExportStateChanged() {
  if (host_ == nullptr || !host_->import_export()->export_inflight()) {
    // The end of an export is published by ExportFinished.
    return;
  }
  if (running_export_id_.isEmpty()) {
    running_export_id_ = QStringLiteral("export-%1").arg(++export_count_);
    last_export_id_    = running_export_id_;
  }
  emit TaskUpdated(RunningExportTask());
}

void AutomationTaskTracker::OnExportFinished(int succeeded, int failed, int skipped) {
  (void)succeeded;
  (void)skipped;
  if (host_ == nullptr || running_export_id_.isEmpty()) {
    return;
  }
  const ui::ImportExportHandler& import_export = *host_->import_export();
  QJsonObject                    task{{"task_id", running_export_id_},
                                      {"kind", "export"},
                                      {"state", failed > 0 ? "failed" : "succeeded"},
                                      {"progress", 100},
                                      {"message", ExportMessage(import_export)},
                                      {"counts", ExportCounts(import_export)}};
  finished_tasks_[running_export_id_] = task;
  running_export_id_.clear();
  emit TaskFinished(task);
}

void AutomationTaskTracker::OnBackgroundTasksChanged() {
  if (host_ == nullptr) {
    return;
  }
  for (const QVariant& value : host_->background_tasks()->Tasks()) {
    const QJsonObject task  = BackgroundTaskObject(value.toMap());
    const QString     id    = task.value("task_id").toString();
    const QString     state = task.value("state").toString();
    auto              known = background_states_.find(id);
    // A running task publishes every change (its progress); other states publish once.
    if (known != background_states_.end() && known->second == state &&
        state != QStringLiteral("running")) {
      continue;
    }
    background_states_[id] = state;
    if (IsTerminalState(state)) {
      const QJsonObject finished = WithPasteResult(task);
      if (finished.contains("result")) {
        finished_tasks_[id] = finished;
      }
      emit TaskFinished(finished);
    } else {
      emit TaskUpdated(task);
    }
  }
}

auto AutomationTaskTracker::WithPasteResult(QJsonObject task) const -> QJsonObject {
  if (task.value("kind").toString() != QStringLiteral("adjustmentPaste") ||
      !IsTerminalState(task.value("state").toString())) {
    return task;
  }
  ui::AdjustmentTransferController* transfer = host_->adjustment_transfer();
  if (transfer == nullptr) {
    return task;
  }
  if (const auto result =
          transfer->apply_coordinator()->FinishedResult(task.value("task_id").toString())) {
    task.insert("result", PasteResultObject(*result));
  }
  return task;
}

auto AutomationTaskTracker::Task(const QString& task_id) const -> std::optional<QJsonObject> {
  if (host_ == nullptr) {
    return std::nullopt;
  }
  if (!running_import_id_.isEmpty() && task_id == running_import_id_) {
    return RunningImportTask();
  }
  if (const auto finished = finished_tasks_.find(task_id); finished != finished_tasks_.end()) {
    return finished->second;
  }
  if (!running_export_id_.isEmpty() && task_id == running_export_id_) {
    return RunningExportTask();
  }
  for (const QVariant& value : host_->background_tasks()->Tasks()) {
    const QVariantMap row = value.toMap();
    if (row.value(QStringLiteral("id")).toString() == task_id) {
      return BackgroundTaskObject(row);
    }
  }
  return std::nullopt;
}

auto AutomationTaskTracker::RunningTasks() const -> QJsonArray {
  QJsonArray tasks;
  if (host_ == nullptr) {
    return tasks;
  }
  if (!running_import_id_.isEmpty()) {
    tasks.push_back(RunningImportTask());
  }
  if (!running_export_id_.isEmpty()) {
    tasks.push_back(RunningExportTask());
  }
  for (const QVariant& value : host_->background_tasks()->Tasks()) {
    const QJsonObject task = BackgroundTaskObject(value.toMap());
    if (!IsTerminalState(task.value("state").toString())) {
      tasks.push_back(task);
    }
  }
  return tasks;
}

}  // namespace alcedo::automation
