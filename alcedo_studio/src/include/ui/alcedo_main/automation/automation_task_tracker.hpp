//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <map>
#include <optional>

#include "ui/alcedo_main/album_backend/application_module_host.hpp"

namespace alcedo::automation {

/// Task ids of the automation protocol and the task state that `tasks.list`, `tasks.wait`, and
/// the `task.updated` and `task.finished` notifications report. GUI thread only.
///
/// - A background task of BackgroundTaskController keeps its own id (`bgtask-N`); its state is
///   read from the controller.
/// - ImportExportHandler runs one import at a time and has no task id, so each import run gets
///   the protocol id `import-N` when ImportRunning becomes true. Its state is read from the
///   handler while it runs. When it ends, the final counts are kept for the id, because the
///   handler overwrites them with the next import.
/// - ImportExportHandler runs one export at a time and has no task id either, so each export
///   run gets the protocol id `export-N` when exportInFlight becomes true. ExportFinished
///   ends it; the final counts are kept for the id. An export with a failed photo ends in the
///   state `failed`.
///
/// A task object has `task_id`, `kind`, `state` (`queued`, `running`, `canceling`,
/// `succeeded`, `failed`, or `canceled`), `progress` (percent, -1 when unknown), and `message`.
/// An import task also has `counts` (`total`, `completed`, `failed`, `unsupported`,
/// `excluded`); an export task has `counts` (`total`, `completed`, `succeeded`, `failed`,
/// `skipped`). A finished adjustment Paste task (`adjustmentPaste`) has `result`
/// (`applied_count`, `failure_count`, `applied_ids`, and `failures` with `element_id` and
/// `message`), read from AdjustmentTransferApplyCoordinator::FinishedResult. The tracker keeps
/// that final state for the id, because the controller drops old finished tasks.
class AutomationTaskTracker final : public QObject {
  Q_OBJECT

 public:
  explicit AutomationTaskTracker(ui::ApplicationModuleHost* host, QObject* parent = nullptr);

  /// The id of the import that runs now, or an empty string when no import runs.
  [[nodiscard]] auto        RunningImportId() const -> QString { return running_import_id_; }
  /// The id of the export that runs now, or an empty string when no export runs.
  [[nodiscard]] auto        RunningExportId() const -> QString { return running_export_id_; }
  /// The id of the last export run (running or finished), or an empty string.
  [[nodiscard]] auto        LastExportId() const -> QString { return last_export_id_; }
  /// Records an import of zero files, which ImportExportHandler does not start, as a finished
  /// import task with zero counts. Returns its id.
  auto                      RecordEmptyImport() -> QString;
  /// The state of @p task_id; nothing for an unknown id or a background task that the
  /// controller no longer keeps.
  [[nodiscard]] auto        Task(const QString& task_id) const -> std::optional<QJsonObject>;
  /// The tasks that have not reached a terminal state.
  [[nodiscard]] auto        RunningTasks() const -> QJsonArray;
  [[nodiscard]] static auto IsTerminalState(const QString& state) -> bool;

 signals:
  /// A task changed and has not reached a terminal state.
  void TaskUpdated(const QJsonObject& task);
  /// A task reached a terminal state.
  void TaskFinished(const QJsonObject& task);

 private:
  void                                OnImportStateChanged();
  void                                OnExportStateChanged();
  void                                OnExportFinished(int succeeded, int failed, int skipped);
  void                                OnBackgroundTasksChanged();
  [[nodiscard]] auto                  RunningImportTask() const -> QJsonObject;
  [[nodiscard]] auto                  RunningExportTask() const -> QJsonObject;
  [[nodiscard]] auto                  NextImportId() -> QString;
  /// @p task with the per-photo `result` of its Paste when @p task is a finished Paste.
  [[nodiscard]] auto                  WithPasteResult(QJsonObject task) const -> QJsonObject;

  QPointer<ui::ApplicationModuleHost> host_;
  QString                             running_import_id_;
  int                                 import_count_ = 0;
  QString                             running_export_id_;
  QString                             last_export_id_;
  int                                 export_count_ = 0;
  /// Final state of each finished import, export, and adjustment Paste, by id.
  std::map<QString, QJsonObject>      finished_tasks_;
  /// Last published state of each background task, to publish only changes.
  std::map<QString, QString>          background_states_;
};

}  // namespace alcedo::automation
