// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
#include "ui/alcedo_main/album_backend/project_maintenance_controller.hpp"

#include <QMetaObject>
#include <QMetaType>
#include <QUrl>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>

namespace alcedo::ui {
namespace {

auto ReadSourceChangedEntries(const ProjectService& project) -> QVariantMap {
  QVariantMap result;
  for (const auto id : project.GetStorage()->GetSourceFileStore().ReadChangedEntryIds()) {
    result.insert(QString::number(id), true);
  }
  return result;
}

}  // namespace

ProjectMaintenanceController::ProjectMaintenanceController(
    ProjectMaintenanceService& service, ProjectModule& project, ProjectLaunchCoordinator& launch,
    EditorSessionController& editor, BackgroundTaskController& tasks,
    InteractionPolicyController& policy, QObject* parent, MaintenanceOperationHooks hooks)
    : QObject(parent),
      service_(service),
      project_(project),
      launch_(launch),
      editor_(editor),
      tasks_(tasks),
      policy_(policy),
      model_(service, this),
      hooks_(std::move(hooks)) {
  preparation_timer_.setInterval(25);
  connect(&preparation_timer_, &QTimer::timeout, this,
          &ProjectMaintenanceController::ContinuePreparation);
  connect(&project_, &ProjectModule::ProjectChanged, this, [this] {
    if (!Scanning()) {
      model_.Clear();
      service_.Clear();
    }
    if (!busy_) {
      state_ = QStringLiteral("ready");
      message_.clear();
      error_.clear();
      backup_path_.clear();
      backup_verified_ = false;
      removed_count_ = retained_count_ = 0;
      processed_entries_               = 0;
      progress_determinate_            = false;
      removals_.clear();
      operation_.reset();
    }
    closing_requested_ = false;
    RefreshSourceChangedEntries();
    emit Changed();
  });
  connect(&project_, &ProjectModule::ProjectLoadStateChanged, this, [this] {
    emit Changed();
    if (opening_ && !project_.ProjectLoading()) OnProjectLoadFinished();
  });
  connect(&launch_, &ProjectLaunchCoordinator::LaunchRequestFinished, this, [this](bool started) {
    if (opening_ && !started) {
      opening_ = false;
      if (restoring_)
        Fail(original_error_ + QStringLiteral("\n") + project_.ServiceMessage(), true);
      else
        Restore(project_.ServiceMessage());
    }
  });
}
ProjectMaintenanceController::~ProjectMaintenanceController() { Shutdown(); }
void ProjectMaintenanceController::JoinWorker() {
  if (worker_.joinable()) worker_.join();
}
void ProjectMaintenanceController::Shutdown() {
  canceled_ = true;
  preparation_timer_.stop();
  JoinWorker();
}
bool ProjectMaintenanceController::Available() const {
  return project_.handler().project() && project_.ServiceReady() && !project_.ProjectLoading() &&
         !closing_requested_;
}
QString ProjectMaintenanceController::BackupPath() const { return backup_path_; }
QString ProjectMaintenanceController::ProjectPath() const {
  return QString::fromStdWString(project_.handler().package_path().wstring());
}
void ProjectMaintenanceController::RefreshSourceChangedEntries() {
  QVariantMap marks;
  try {
    if (const auto project = project_.handler().project()) {
      marks = ReadSourceChangedEntries(*project);
    }
  } catch (const std::exception& error) {
    error_ = QString::fromUtf8(error.what());
    emit Changed();
  }
  if (marks != source_changed_entries_) {
    source_changed_entries_ = std::move(marks);
    emit SourceChangedEntriesChanged();
  }
}
bool ProjectMaintenanceController::Scan(bool mark_changed_sources) {
  if (busy_ || !Available()) return false;
  if (mark_changed_sources) {
    if (const auto reason = UserStateChangeRefusal(&policy_); !reason.isEmpty()) {
      error_ = reason;
      emit Changed();
      return false;
    }
  }
  JoinWorker();
  model_.Clear();
  service_.Clear();
  operation_.reset();
  backup_path_.clear();
  backup_verified_ = false;
  canceled_        = false;
  busy_            = true;
  state_           = QStringLiteral("scanning");
  error_.clear();
  removed_count_ = retained_count_ = 0;
  processed_entries_               = 0;
  progress_determinate_            = false;
  removals_.clear();
  message_ = tr("Scanning ordinary photos");
  BackgroundTaskSnapshot task;
  task.kind_             = BackgroundTaskKind::MaintenanceScan;
  task.state_            = BackgroundTaskState::Running;
  task.title_            = tr("Project Maintenance");
  task.detail_           = message_;
  task.progress_percent_ = -1;
  task.cancelable_       = true;
  task_id_               = tasks_.RegisterTask(task, [this] { CancelScan(); });
  const auto project     = project_.handler().project();
  const auto journal     = project_.handler().editor_journal_directory();
  const auto active =
      editor_.has_image() ? std::optional<sl_element_id_t>(editor_.element_id()) : std::nullopt;
  auto save = mark_changed_sources ? project_.handler().MakeSaveAndPackageJob()
                                   : std::function<bool(QString*)>{};
  worker_   = std::thread([this, project, journal, active, save = std::move(save)] {
    bool    complete = false;
    QString failure;
    bool    refresh_marks = false;
    try {
      complete = service_.Scan(
          project->GetStorage()->GetProjectMaintenanceStore(), journal, active,
          [this] { return canceled_.load(); },
          [this](size_t count) {
            if (count % 64 == 0)
              QMetaObject::invokeMethod(
                  this,
                  [this, count] {
                    message_ = tr("Inspected %1 entries").arg(count);
                    tasks_.UpdateTask(task_id_, tr("Project Maintenance"), message_, -1);
                    emit Changed();
                  },
                  Qt::QueuedConnection);
          },
          {}, static_cast<bool>(save));
      if (complete && service_.ChangedMarksWritten()) {
        refresh_marks = true;
        QString save_error;
        if (!save(&save_error)) {
          throw std::runtime_error(save_error.isEmpty()
                                       ? tr("Could not save source-change marks").toStdString()
                                       : save_error.toStdString());
        }
      }
    } catch (const std::exception& error) {
      failure = QString::fromUtf8(error.what());
      service_.Clear();
      complete = false;
    }
    QMetaObject::invokeMethod(
        this,
        [this, complete, failure, refresh_marks] {
          JoinWorker();
          busy_  = false;
          error_ = failure;
          if (refresh_marks) RefreshSourceChangedEntries();
          state_ = failure.isEmpty()
                       ? (complete ? QStringLiteral("review") : QStringLiteral("canceled"))
                       : QStringLiteral("failed");
          model_.Refresh();
          message_ =
              complete ? (service_.Findings().empty() ? tr("No duplicate or invalid entries found")
                                                      : tr("%1 findings. Select entries to remove.")
                                                            .arg(service_.Findings().size()))
                       : (failure.isEmpty() ? tr("Scan canceled") : QString{});
          tasks_.FinishTask(task_id_,
                            !failure.isEmpty() ? BackgroundTaskState::Failed
                            : complete         ? BackgroundTaskState::Succeeded
                                               : BackgroundTaskState::Canceled,
                            failure.isEmpty() ? message_ : failure);
          emit Changed();
          emit ScanFinished(complete);
        },
        Qt::QueuedConnection);
  });
  emit Changed();
  return true;
}
void ProjectMaintenanceController::CancelScan() {
  if (Scanning()) canceled_ = true;
}
bool ProjectMaintenanceController::ApplySelected(const QString& backup_directory) {
  if (const auto reason = UserStateChangeRefusal(&policy_); !reason.isEmpty()) {
    error_ = reason;
    emit Changed();
    return false;
  }
  return BeginApply(model_.SelectedIds(), backup_directory);
}
bool ProjectMaintenanceController::ApplyIds(const QVariantList& values,
                                            const QString&      backup_directory) {
  std::vector<sl_element_id_t> ids;
  for (const auto& value : values) {
    const auto type    = value.typeId();
    const bool numeric = type == QMetaType::Int || type == QMetaType::UInt ||
                         type == QMetaType::LongLong || type == QMetaType::ULongLong ||
                         type == QMetaType::Long || type == QMetaType::ULong ||
                         type == QMetaType::Double || type == QMetaType::Float;
    const auto id      = numeric ? value.toDouble() : 0.0;
    if (!numeric || !std::isfinite(id) || id < 1 ||
        id > std::numeric_limits<sl_element_id_t>::max() || std::floor(id) != id) {
      error_ = tr("Entry IDs must be nonzero 32-bit integers.");
      emit Changed();
      return false;
    }
    ids.push_back(static_cast<sl_element_id_t>(id));
  }
  return BeginApply(std::move(ids), backup_directory);
}
bool ProjectMaintenanceController::BeginApply(std::vector<sl_element_id_t> ids,
                                              const QString&               backup_directory) {
  if (busy_ || !Available()) return false;
  try {
    removals_ = service_.Selection(ids);
  } catch (const std::exception& error) {
    error_ = QString::fromUtf8(error.what());
    emit Changed();
    return false;
  }
  JoinWorker();
  const QUrl backup_url(backup_directory);
  backup_directory_ = std::filesystem::path(
      (backup_url.isLocalFile() ? backup_url.toLocalFile() : backup_directory).toStdWString());
  original_path_     = project_.handler().package_path();
  busy_              = true;
  editor_finalized_  = false;
  restoring_         = false;
  closing_requested_ = false;
  removed_count_ = retained_count_ = 0;
  processed_entries_               = 0;
  progress_determinate_            = false;
  state_                           = QStringLiteral("preparing");
  error_.clear();
  original_error_.clear();
  message_ = tr("Waiting for existing project work");
  model_.SetReadOnly(true);
  BackgroundTaskSnapshot task;
  task.kind_             = BackgroundTaskKind::MaintenanceApply;
  task.state_            = BackgroundTaskState::Running;
  task.title_            = tr("Project Maintenance");
  task.detail_           = message_;
  task.progress_percent_ = -1;
  task.shutdown_policy_  = BackgroundTaskShutdownPolicy::WaitForFinish;
  for (auto capability :
       {InteractionCapability::UserStateChange, InteractionCapability::EditImageDescription,
        InteractionCapability::EditImageRating, InteractionCapability::EditImageRatingReason,
        InteractionCapability::DeleteImages, InteractionCapability::ChangeSemanticModel,
        InteractionCapability::RunSemanticGeneration, InteractionCapability::SelectEditorImage,
        InteractionCapability::SwitchWorkspace, InteractionCapability::CheckoutVersion,
        InteractionCapability::PasteAdjustments, InteractionCapability::CloseProject}) {
    task.locks_.push_back(
        {capability, 0, tr("Project Maintenance is applying reviewed removals.")});
  }
  task_id_ = tasks_.RegisterTask(task);
  preparation_timer_.start();
  emit Changed();
  return true;
}
void ProjectMaintenanceController::ContinuePreparation() {
  if (tasks_.ActiveCountExcept(BackgroundTaskKind::AgentControl) > 1) return;
  if (!editor_finalized_) {
    editor_finalized_ = true;
    if (editor_.has_image() || editor_.active()) {
      editor_.Finalize(true);
      return;
    }
  }
  if (editor_.close_in_flight() || editor_.persist_in_flight()) return;
  if (editor_.has_image() || editor_.has_pending_recovery()) {
    preparation_timer_.stop();
    Fail(editor_.last_error());
    return;
  }
  preparation_timer_.stop();
  try {
    project_.handler().pipeline_service()->Sync();
  } catch (const std::exception& error) {
    Fail(QString::fromUtf8(error.what()));
    return;
  }
  state_   = QStringLiteral("applying");
  message_ = tr("Saving and verifying the maintenance backup");
  operation_ =
      hooks_.create_package
          ? hooks_.create_package()
          : std::make_shared<ProjectMaintenancePackage>(
                [this](auto stage) {
                  QMetaObject::invokeMethod(
                      this,
                      [this, stage] {
                        progress_determinate_ = stage == MaintenancePackageStage::Removal;
                        switch (stage) {
                          case MaintenancePackageStage::Backup:
                            message_ = tr("Creating the maintenance backup");
                            break;
                          case MaintenancePackageStage::Removal:
                            message_ = tr("Removing reviewed entries in the working package");
                            break;
                          case MaintenancePackageStage::ResultWrite:
                            message_ = tr("Writing and reopening the result package");
                            break;
                          case MaintenancePackageStage::Publication:
                            message_ = tr("Publishing the verified result");
                            break;
                          case MaintenancePackageStage::Restoration:
                            message_ = tr("Restoring the verified backup");
                            break;
                        }
                        tasks_.UpdateTask(task_id_, tr("Project Maintenance"), message_, -1);
                        emit Changed();
                      },
                      Qt::QueuedConnection);
                },
                [this](size_t completed, size_t total) {
                  // Bound queued UI work while preserving the first and final count.
                  if (completed != 0 && completed != total && completed % 16 != 0) return;
                  QMetaObject::invokeMethod(
                      this,
                      [this, completed, total] {
                        processed_entries_ = static_cast<int>(completed);
                        const int percent  = static_cast<int>(100 * completed / total);
                        tasks_.UpdateTask(task_id_, tr("Project Maintenance"), message_, percent);
                        emit Changed();
                      },
                      Qt::QueuedConnection);
                });
  auto job = project_.handler().MakeMaintenanceJob(operation_, removals_, backup_directory_);
  worker_  = std::thread([this, job = std::move(job)]() mutable {
    QString failure;
    try {
      job();
    } catch (const std::exception& error) {
      failure = QString::fromUtf8(error.what());
    }
    job = {};  // Release live project references before activation on the UI thread.
    QMetaObject::invokeMethod(
        this,
        [this, failure] {
          JoinWorker();
          backup_path_     = QString::fromStdWString(operation_->BackupPath().wstring());
          backup_verified_ = operation_->BackupVerified();
          if (!failure.isEmpty()) {
            if (operation_->Installed())
              Restore(failure);
            else
              Fail(failure, operation_->RecoveryRequired());
          } else
            OpenPublishedProject();
        },
        Qt::QueuedConnection);
  });
  emit Changed();
}
void ProjectMaintenanceController::OpenPublishedProject() {
  progress_determinate_ = false;
  state_                = restoring_ ? QStringLiteral("restoring") : QStringLiteral("reopening");
  message_              = tr("Reopening the verified project");
  lifecycle_change_     = true;
  const bool closed     = !project_.handler().project() || project_.CloseProject(false);
  if (!closed) {
    Restore(project_.ServiceMessage());
    return;
  }
  opening_        = true;
  const auto path = QString::fromStdWString(original_path_.wstring());
  const bool started =
      hooks_.reopen_project ? hooks_.reopen_project(path) : launch_.BeginOpen(path);
  if (!started) {
    opening_ = false;
    if (restoring_)
      Fail(tr("Backup restored, but project reopening could not start."), true);
    else
      Restore(tr("Cleaned project reopening could not start."));
  }
  emit Changed();
}
void ProjectMaintenanceController::OnProjectLoadFinished() {
  lifecycle_change_ = false;
  opening_          = false;
  if (!project_.ServiceReady() || !project_.handler().project()) {
    if (restoring_)
      Fail(original_error_ + QStringLiteral("\n") + project_.ServiceMessage(), true);
    else
      Restore(project_.ServiceMessage());
    return;
  }
  try {
    operation_->VerifyActivatedProject(*project_.handler().project());
  } catch (const std::exception& error) {
    if (restoring_)
      Fail(original_error_ + QStringLiteral("\n") + QString::fromUtf8(error.what()), true);
    else
      Restore(QString::fromUtf8(error.what()));
    return;
  }
  operation_->Finish();
  if (restoring_) {
    Fail(original_error_ + tr("\nThe verified backup was restored and reopened."));
    return;
  }
  busy_           = false;
  state_          = QStringLiteral("complete");
  removed_count_  = static_cast<int>(operation_->RemovedIds().size());
  retained_count_ = static_cast<int>(
      project_.handler().project()->GetStorage()->GetElementStore().CountFilesInFolder(0));
  message_ = tr("Removed %1 project entries. Retained %2 entries.")
                 .arg(removed_count_)
                 .arg(retained_count_);
  tasks_.FinishTask(task_id_, BackgroundTaskState::Succeeded, message_);
  emit Changed();
  emit Finished(true);
}
void ProjectMaintenanceController::Restore(const QString& reason) {
  progress_determinate_ = false;
  if (restoring_) {
    Fail(original_error_ + QStringLiteral("\n") + reason, true);
    return;
  }
  original_error_ = reason;
  restoring_      = true;
  auto job        = project_.handler().MakeMaintenanceRestoreJob(operation_);
  JoinWorker();
  worker_ = std::thread([this, job = std::move(job)] {
    QString failure;
    try {
      job();
    } catch (const std::exception& error) {
      failure = QString::fromUtf8(error.what());
    }
    QMetaObject::invokeMethod(
        this,
        [this, failure] {
          JoinWorker();
          if (!failure.isEmpty())
            Fail(original_error_ + QStringLiteral("\n") + failure, true);
          else
            OpenPublishedProject();
        },
        Qt::QueuedConnection);
  });
}
void ProjectMaintenanceController::Fail(const QString& reason, bool recovery) {
  progress_determinate_ = false;
  lifecycle_change_     = false;
  removed_count_ = retained_count_ = 0;
  error_                           = reason.isEmpty() ? tr("Project Maintenance failed") : reason;
  state_ = recovery ? QStringLiteral("recovery") : QStringLiteral("failed");
  busy_  = recovery;
  model_.Clear();
  service_.Clear();
  if (recovery)
    tasks_.UpdateTask(task_id_, tr("Project recovery required"), error_, -1);
  else
    tasks_.FinishTask(task_id_, BackgroundTaskState::Failed, error_);
  emit Changed();
  emit Finished(false);
}
void ProjectMaintenanceController::RequestProjectClose() {
  closing_requested_ = true;
  CancelScan();
  emit Changed();
}
QString ProjectMaintenanceController::ProjectSwitchBlockReason() const {
  return busy_ && !lifecycle_change_ && !closing_requested_
             ? tr("Wait for Project Maintenance to finish.")
             : QString{};
}
}  // namespace alcedo::ui
