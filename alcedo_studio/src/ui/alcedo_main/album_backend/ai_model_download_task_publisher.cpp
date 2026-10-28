//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/ai_model_download_task_publisher.hpp"

#include <QPointer>
#include <cmath>

namespace alcedo::ui {

AiModelDownloadTaskPublisher::AiModelDownloadTaskPublisher(alcedo::AiModelPackageService& packages,
                                                           BackgroundTaskController&      tasks,
                                                           QObject*                       parent)
    : QObject(parent), packages_(packages), tasks_(tasks) {
  connect(&packages_, &alcedo::AiModelPackageService::InstallStarted, this,
          &AiModelDownloadTaskPublisher::OnInstallStarted);
  connect(&packages_, &alcedo::AiModelPackageService::PackageChanged, this,
          &AiModelDownloadTaskPublisher::OnPackageChanged);
  connect(&packages_, &alcedo::AiModelPackageService::InstallFinished, this,
          &AiModelDownloadTaskPublisher::OnInstallFinished);
}

auto AiModelDownloadTaskPublisher::Title(const QString& package_id) const -> QString {
  const alcedo::AiModelPackageService::PackageState* package = packages_.Package(package_id);
  const QString name = package_id == QLatin1String(alcedo::kAiMaskModelPackageId.data(),
                                                   alcedo::kAiMaskModelPackageId.size())
                           ? tr("AI Mask models")
                           : package_id;
  if (package == nullptr) return name;
  switch (package->status) {
    case alcedo::AiModelPackageStatus::kVerifying:
      return tr("Verifying %1").arg(name);
    case alcedo::AiModelPackageStatus::kInstalling:
      return tr("Installing %1").arg(name);
    default:
      return tr("Downloading %1").arg(name);
  }
}

void AiModelDownloadTaskPublisher::OnInstallStarted(const QString& package_id) {
  BackgroundTaskSnapshot snapshot;
  snapshot.kind_             = BackgroundTaskKind::ModelDownload;
  snapshot.state_            = BackgroundTaskState::Running;
  snapshot.title_            = Title(package_id);
  snapshot.progress_percent_ = 0;
  snapshot.cancelable_       = true;
  snapshot.shutdown_policy_  = BackgroundTaskShutdownPolicy::CancelAndWait;
  snapshot.affected_targets_ = QVariantList{package_id};
  snapshot.locks_.push_back(
      InteractionLock{InteractionCapability::ChangeModelDownloadSettings, 0,
                      tr("An AI model download is running; wait for it to finish or cancel it.")});
  QPointer<alcedo::AiModelPackageService> packages(&packages_);
  task_ids_.insert(package_id, tasks_.RegisterTask(snapshot, [packages, package_id] {
    if (packages) packages->CancelInstall(package_id);
  }));
}

void AiModelDownloadTaskPublisher::OnPackageChanged(const QString& package_id) {
  const QString task_id = task_ids_.value(package_id);
  if (task_id.isEmpty()) return;
  const alcedo::AiModelPackageService::PackageState* package = packages_.Package(package_id);
  if (package == nullptr) return;
  // Verification and extraction report no fraction; the bar is indeterminate then.
  const int percent = package->status == alcedo::AiModelPackageStatus::kDownloading
                          ? static_cast<int>(std::lround(package->progress * 100.0))
                          : -1;
  tasks_.UpdateTask(task_id, Title(package_id), QString(), percent);
}

void AiModelDownloadTaskPublisher::OnInstallFinished(const QString& package_id, bool installed,
                                                     bool canceled, const QString& message) {
  const QString task_id = task_ids_.take(package_id);
  if (task_id.isEmpty()) return;
  const BackgroundTaskState state = installed  ? BackgroundTaskState::Succeeded
                                    : canceled ? BackgroundTaskState::Canceled
                                               : BackgroundTaskState::Failed;
  tasks_.FinishTask(task_id, state, installed ? tr("AI models installed.") : message);
}

}  // namespace alcedo::ui
