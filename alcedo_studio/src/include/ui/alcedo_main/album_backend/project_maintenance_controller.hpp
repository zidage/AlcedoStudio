// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QObject>
#include <QTimer>
#include <QVariantMap>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "app/project_maintenance_package.hpp"
#include "ui/alcedo_main/album_backend/background_task_controller.hpp"
#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"
#include "ui/alcedo_main/album_backend/interaction_policy_controller.hpp"
#include "ui/alcedo_main/album_backend/project_launch_coordinator.hpp"
#include "ui/alcedo_main/album_backend/project_maintenance_model.hpp"

namespace alcedo::ui {
/// Explicit package and activation boundaries for deterministic failure qualification.
struct MaintenanceOperationHooks {
  std::function<std::shared_ptr<ProjectMaintenancePackage>()> create_package;
  std::function<bool(const QString&)>                         reopen_project;
};
/// Application-owned maintenance lifecycle. UI thread publishes state and selection; one
/// worker performs scoped storage/package operations. Success requires project activation.
class ProjectMaintenanceController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(ProjectMaintenanceModel* model READ Model CONSTANT)
  Q_PROPERTY(bool available READ Available NOTIFY Changed)
  Q_PROPERTY(bool busy READ Busy NOTIFY Changed)
  Q_PROPERTY(bool scanning READ Scanning NOTIFY Changed)
  Q_PROPERTY(QString state READ State NOTIFY Changed)
  Q_PROPERTY(QString message READ Message NOTIFY Changed)
  Q_PROPERTY(QString error READ Error NOTIFY Changed)
  Q_PROPERTY(QString backupPath READ BackupPath NOTIFY Changed)
  Q_PROPERTY(QString projectPath READ ProjectPath NOTIFY Changed)
  Q_PROPERTY(bool backupVerified READ BackupVerified NOTIFY Changed)
  Q_PROPERTY(int removedCount READ RemovedCount NOTIFY Changed)
  Q_PROPERTY(int retainedCount READ RetainedCount NOTIFY Changed)
  Q_PROPERTY(int processedEntries READ ProcessedEntries NOTIFY Changed)
  Q_PROPERTY(int totalEntries READ TotalEntries NOTIFY Changed)
  Q_PROPERTY(bool progressDeterminate READ ProgressDeterminate NOTIFY Changed)
  Q_PROPERTY(
      QVariantMap sourceChangedEntries READ SourceChangedEntries NOTIFY SourceChangedEntriesChanged)
 public:
  ProjectMaintenanceController(ProjectMaintenanceService& service, ProjectModule& project,
                               ProjectLaunchCoordinator& launch, EditorSessionController& editor,
                               BackgroundTaskController& tasks, InteractionPolicyController& policy,
                               QObject* parent = nullptr, MaintenanceOperationHooks hooks = {});
  ~ProjectMaintenanceController() override;
  ProjectMaintenanceModel* Model() { return &model_; }
  bool                     Available() const;
  bool                     Busy() const { return busy_; }
  bool                     Scanning() const { return state_ == QStringLiteral("scanning"); }
  QString                  State() const { return state_; }
  QString                  Message() const { return message_; }
  QString                  Error() const { return error_; }
  QString                  BackupPath() const;
  /// Current package path, read through ProjectHandler on the UI thread.
  QString                  ProjectPath() const;
  bool                     BackupVerified() const { return backup_verified_; }
  int                      RemovedCount() const { return removed_count_; }
  int                      RetainedCount() const { return retained_count_; }
  /// Transaction work progress, not the durable RemovedCount result. UI thread only.
  int                      ProcessedEntries() const { return processed_entries_; }
  int                      TotalEntries() const { return static_cast<int>(removals_.size()); }
  bool                     ProgressDeterminate() const { return progress_determinate_; }
  /// Starts a scan. The review panel explicitly enables saved Library marks.
  Q_INVOKABLE bool         Scan(bool mark_changed_sources = false);
  /// Saved source-change marks projected into the Library by decimal element ID.
  auto SourceChangedEntries() const -> QVariantMap { return source_changed_entries_; }
  /// Refresh the Library projection after opening a project or changing entry ownership.
  void RefreshSourceChangedEntries();
  Q_INVOKABLE void         CancelScan();
  /// UI calls only after explicit confirmation. Refuses when another agent holds control.
  Q_INVOKABLE bool         ApplySelected(const QString& backup_directory = {});
  /// Commands supply explicit IDs; identical selection/backup/preservation rules apply.
  Q_INVOKABLE bool         ApplyIds(const QVariantList& ids, const QString& backup_directory = {});
  /// Cancel and settle a scan, or await apply, before a coordinated project close.
  void                     RequestProjectClose();
  void                     CancelProjectCloseRequest() {
    closing_requested_ = false;
    emit Changed();
  }
  QString ProjectSwitchBlockReason() const;
  /// Cancel a scan and join worker before releasing the project owners. UI thread only.
  void    Shutdown();
 signals:
  void Changed();
  void ScanFinished(bool complete);
  void SourceChangedEntriesChanged();
  void Finished(bool success);

 private:
  /// UI thread: validate reviewed IDs and resolve the backup folder/local file URL, then
  /// reserve writes and prepare. Returns false for busy/unavailable state or invalid IDs.
  bool BeginApply(std::vector<sl_element_id_t> ids, const QString& backup_directory);
  /// UI preparation timer: drain admitted tasks and finalize/save the editor before starting
  /// the package worker. Preparation errors call Fail; writes stay reserved during activation.
  void ContinuePreparation();
  /// UI thread after publication/restoration: close without saving and reopen original_path_.
  /// Requires operation_; launch failure restores the backup or reports unresolved recovery.
  void OpenPublishedProject();
  /// UI load-completion callback: verify activation before releasing artifacts/task locks.
  /// Invalid activation starts restoration; restored activation reports the original failure.
  void OnProjectLoadFinished();
  /// UI thread with operation_ set: retain the original error and restore on the worker, then
  /// reopen. Repeated restoration or a worker failure keeps writes reserved for recovery.
  void Restore(const QString& reason);
  /// UI thread: clear review/results and emit Finished(false) with the supplied error.
  /// recovery=true keeps busy/task write restrictions; false finishes the failed task.
  void Fail(const QString& reason, bool recovery = false);
  /// UI completion/shutdown path: wait for worker return without publishing state changes.
  /// Never call on worker_; its UI notifications are queued and do not wait for this thread.
  void JoinWorker();
  ProjectMaintenanceService&                 service_;
  ProjectModule&                             project_;
  ProjectLaunchCoordinator&                  launch_;
  EditorSessionController&                   editor_;
  BackgroundTaskController&                  tasks_;
  InteractionPolicyController&               policy_;
  ProjectMaintenanceModel                    model_;
  QVariantMap                                source_changed_entries_;
  QTimer                                     preparation_timer_;
  std::thread                                worker_;
  std::atomic<bool>                          canceled_{false};
  std::shared_ptr<ProjectMaintenancePackage> operation_;
  std::vector<MaintenanceRemoval>            removals_;
  std::filesystem::path                      backup_directory_, original_path_;
  QString task_id_, state_ = QStringLiteral("ready"), message_, error_, original_error_;
  QString backup_path_;
  bool    backup_verified_ = false;
  int     removed_count_ = 0, retained_count_ = 0;
  int     processed_entries_    = 0;
  bool    progress_determinate_ = false;
  bool    busy_ = false, editor_finalized_ = false, opening_ = false, restoring_ = false;
  bool    lifecycle_change_ = false, closing_requested_ = false;
  MaintenanceOperationHooks hooks_;
};
}  // namespace alcedo::ui
