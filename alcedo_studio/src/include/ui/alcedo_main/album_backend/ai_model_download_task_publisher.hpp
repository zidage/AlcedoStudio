//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QHash>
#include <QObject>
#include <QString>

#include "app/ai_model_package_service.hpp"
#include "ui/alcedo_main/album_backend/background_task_controller.hpp"

namespace alcedo::ui {

/// Publishes each AI model package install of AiModelPackageService as a cancelable
/// `ModelDownload` background task: registered when the download starts, updated with the
/// stage and download progress, and finished with the install result. Canceling the task
/// cancels the install. While the task runs it holds the global `ChangeModelDownloadSettings`
/// lock, so the model directory does not change under the install.
///
/// Thread affinity: owner (GUI) thread, the thread of both services.
class AiModelDownloadTaskPublisher final : public QObject {
  Q_OBJECT

 public:
  AiModelDownloadTaskPublisher(alcedo::AiModelPackageService& packages,
                               BackgroundTaskController& tasks, QObject* parent = nullptr);

  /// Background task ID of the running install of @p package_id; empty when none runs.
  [[nodiscard]] auto TaskId(const QString& package_id) const -> QString {
    return task_ids_.value(package_id);
  }

 private:
  void               OnInstallStarted(const QString& package_id);
  void               OnPackageChanged(const QString& package_id);
  void               OnInstallFinished(const QString& package_id, bool installed, bool canceled,
                                       const QString& message);
  [[nodiscard]] auto Title(const QString& package_id) const -> QString;

  alcedo::AiModelPackageService& packages_;
  BackgroundTaskController&      tasks_;
  QHash<QString, QString>        task_ids_;
};

}  // namespace alcedo::ui
