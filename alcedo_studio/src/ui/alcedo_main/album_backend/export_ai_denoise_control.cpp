//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/export_ai_denoise_control.hpp"

#include <QMetaObject>
#include <QThreadPool>
#include <exception>
#include <utility>

#include "ui/alcedo_main/i18n.hpp"
#include "utils/logging/log.hpp"

namespace alcedo::ui {

ExportAiDenoiseControl::ExportAiDenoiseControl(Services                  services,
                                               BackgroundTaskController* background_tasks,
                                               ExportQueue* export_queue, QObject* parent)
    : QObject(parent),
      services_(std::move(services)),
      background_tasks_(background_tasks),
      export_queue_(export_queue) {}

auto ExportAiDenoiseControl::Refused(const QString& message) -> QVariantMap {
  return QVariantMap{{QStringLiteral("started"), false}, {QStringLiteral("message"), message}};
}

auto ExportAiDenoiseControl::turnOffForExportItem(uint elementId) -> QVariantMap {
  if (services_.user_refusal) {
    if (const QString refusal = services_.user_refusal(); !refusal.isEmpty()) {
      return Refused(refusal);
    }
  }
  if (running_) {
    return Refused(Tr("AI Denoise is already being changed."));
  }
  const auto element_id = static_cast<sl_element_id_t>(elementId);
  if (element_id == 0) {
    return Refused(Tr("No valid image was selected."));
  }

  // The editor session owns the history of the photo it has open.
  if (services_.editor_element_id && services_.editor_element_id() == element_id) {
    const bool accepted = services_.turn_off_editor_photo && services_.turn_off_editor_photo();
    Finish(element_id,
           accepted ? std::optional<AiDenoiseHistoryWrite>{AiDenoiseHistoryWrite{}} : std::nullopt,
           accepted ? QString() : Tr("The editor did not accept the change."));
    return QVariantMap{{QStringLiteral("started"), true}, {QStringLiteral("message"), QString()}};
  }

  StoredHistoryWriter write =
      services_.stored_history_writer ? services_.stored_history_writer() : StoredHistoryWriter{};
  if (!write) {
    return Refused(Tr("Pipeline service is unavailable."));
  }
  running_ = true;
  emit RunningChanged();
  if (background_tasks_ != nullptr) {
    // The write changes a stored history: no other owner may open, delete, or paste into it.
    const QString          reason = Tr("AI Denoise is being changed.");
    BackgroundTaskSnapshot snapshot;
    snapshot.kind_             = BackgroundTaskKind::AiDenoiseSwitch;
    snapshot.state_            = BackgroundTaskState::Running;
    snapshot.title_            = Tr("AI Denoise");
    snapshot.detail_           = Tr("Turn Off AI Denoise");
    snapshot.progress_percent_ = -1;
    snapshot.cancelable_       = false;
    snapshot.shutdown_policy_  = BackgroundTaskShutdownPolicy::WaitForFinish;
    snapshot.affected_targets_.push_back(static_cast<quint64>(element_id));
    snapshot.locks_ = {
        {InteractionCapability::PasteAdjustments, 0, reason},
        {InteractionCapability::CloseProject, 0, reason},
        {InteractionCapability::DeleteImages, 0, reason},
        {InteractionCapability::SelectEditorImage, 0, reason},
        {InteractionCapability::SwitchWorkspace, 0, reason},
    };
    task_id_ = background_tasks_->RegisterTask(snapshot);
  }

  QPointer<ExportAiDenoiseControl> self(this);
  QThreadPool::globalInstance()->start([self, write = std::move(write), element_id]() {
    std::optional<AiDenoiseHistoryWrite> written;
    QString                              error;
    try {
      written = write(element_id, std::nullopt);
    } catch (const std::exception& exception) {
      error = QString::fromStdString(exception.what());
      ALCEDO_LOG_WARN(Render) << exception.what();
    }
    QMetaObject::invokeMethod(
        self.data(),
        [self, element_id, written, error]() {
          if (self) {
            self->Finish(element_id, written, error);
          }
        },
        Qt::QueuedConnection);
  });
  return QVariantMap{{QStringLiteral("started"), true}, {QStringLiteral("message"), QString()}};
}

void ExportAiDenoiseControl::Finish(sl_element_id_t                      element_id,
                                    std::optional<AiDenoiseHistoryWrite> write,
                                    const QString&                       error) {
  const bool success = write.has_value();
  if (success) {
    if (write->changed && services_.refresh_written_photo) {
      services_.refresh_written_photo(element_id, *write);
    }
    if (export_queue_ != nullptr) {
      export_queue_->ReturnToQueued(element_id);
    }
  }
  const QString message =
      success ? QString() : Tr("AI Denoise could not be turned off: %1").arg(error);
  if (background_tasks_ != nullptr && !task_id_.isEmpty()) {
    background_tasks_->FinishTask(
        task_id_, success ? BackgroundTaskState::Succeeded : BackgroundTaskState::Failed, message);
  }
  task_id_.clear();
  if (running_) {
    running_ = false;
    emit RunningChanged();
  }
  const QVariantMap result{{QStringLiteral("elementId"), static_cast<qulonglong>(element_id)},
                           {QStringLiteral("success"), success},
                           {QStringLiteral("message"), message}};
  emit              TurnOffFinished(result);
  emit              turnOffFinished(result);
}

}  // namespace alcedo::ui
