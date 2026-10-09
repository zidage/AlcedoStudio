//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/automation/automation_task_tracker.hpp"

#include <QVariantList>
#include <QVariantMap>
#include <algorithm>

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

}  // namespace

AutomationTaskTracker::AutomationTaskTracker(ui::ApplicationModuleHost* host, QObject* parent)
    : QObject(parent), host_(host) {
  if (host_ == nullptr) {
    return;
  }
  connect(host_->import_export(), &ui::ImportExportHandler::ImportStateChanged, this,
          &AutomationTaskTracker::OnImportStateChanged);
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
  finished_imports_[id] = task;
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
  finished_imports_[running_import_id_] = task;
  running_import_id_.clear();
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
      emit TaskFinished(task);
    } else {
      emit TaskUpdated(task);
    }
  }
}

auto AutomationTaskTracker::Task(const QString& task_id) const -> std::optional<QJsonObject> {
  if (host_ == nullptr) {
    return std::nullopt;
  }
  if (!running_import_id_.isEmpty() && task_id == running_import_id_) {
    return RunningImportTask();
  }
  if (const auto finished = finished_imports_.find(task_id); finished != finished_imports_.end()) {
    return finished->second;
  }
  if (task_id == QStringLiteral("export")) {
    const bool running = host_->import_export()->export_inflight();
    return QJsonObject{{"task_id", task_id},
                       {"kind", "export"},
                       {"state", running ? "running" : "succeeded"},
                       {"progress", -1},
                       {"message", host_->import_export()->ExportStatus()}};
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
  if (host_->import_export()->export_inflight()) {
    tasks.push_back(*Task(QStringLiteral("export")));
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
