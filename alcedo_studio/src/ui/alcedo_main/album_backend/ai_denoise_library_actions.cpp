//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/ai_denoise_library_actions.hpp"

#include <QMetaObject>
#include <QThreadPool>
#include <exception>
#include <memory>
#include <utility>

#include "edit/input/ai_denoise_error.hpp"
#include "ui/alcedo_main/i18n.hpp"
#include "utils/logging/log.hpp"

namespace alcedo::ui {
namespace {

auto PhotoRow(sl_element_id_t element_id, const QString& file_name, const QString& message)
    -> QVariantMap {
  return QVariantMap{{QStringLiteral("elementId"), static_cast<qulonglong>(element_id)},
                     {QStringLiteral("fileName"), file_name},
                     {QStringLiteral("message"), message}};
}

/// The snackbar text of a finished turn-on.
auto TurnOnMessage(int turned_on, int total, int not_supported, int failed) -> QString {
  if (not_supported == 0 && failed == 0) {
    return total == 1 ? Tr("AI Denoise is on.") : Tr("AI Denoise is on for %1 photos.").arg(total);
  }
  QString message = Tr("AI Denoise is on for %1 of %2 photos.").arg(turned_on).arg(total);
  if (not_supported > 0) {
    message += QLatin1Char(' ') + Tr("Not a Bayer RAW photo: %1.").arg(not_supported);
  }
  if (failed > 0) {
    message += QLatin1Char(' ') + Tr("Failed: %1.").arg(failed);
  }
  return message;
}

}  // namespace

AiDenoiseLibraryActions::AiDenoiseLibraryActions(Services                  services,
                                                 BackgroundTaskController* background_tasks,
                                                 ExportQueue* export_queue, QObject* parent)
    : QObject(parent),
      services_(std::move(services)),
      background_tasks_(background_tasks),
      export_queue_(export_queue) {}

auto AiDenoiseLibraryActions::available() const -> bool {
  if (!available_.has_value()) {
    available_ = services_.platform_available && services_.platform_available();
  }
  return *available_;
}

auto AiDenoiseLibraryActions::Refused(const QString& message) const -> QVariantMap {
  return QVariantMap{{QStringLiteral("started"), false},
                     {QStringLiteral("result"), QStringLiteral("refused")},
                     {QStringLiteral("message"), message}};
}

auto AiDenoiseLibraryActions::turnOnForTargets(const QVariantList& targets) -> QVariantMap {
  if (!available()) {
    return Refused(Tr("AI Denoise cannot run on this computer."));
  }
  if (services_.user_refusal) {
    if (const QString refusal = services_.user_refusal(); !refusal.isEmpty()) {
      return Refused(refusal);
    }
  }
  if (running_) {
    return Refused(Tr("AI Denoise is already being changed."));
  }
  std::string version;
  try {
    version = services_.installed_model_version();
  } catch (const AiDenoiseError& error) {
    if (error.Kind() == AiDenoiseErrorKind::ModelNotInstalled) {
      return QVariantMap{{QStringLiteral("started"), false},
                         {QStringLiteral("result"), QStringLiteral("model_not_installed")},
                         {QStringLiteral("message"), QString()}};
    }
    return Refused(QString::fromStdString(error.Text()));
  } catch (const std::exception& error) {
    return Refused(QString::fromStdString(error.what()));
  }

  const sl_element_id_t editor_id =
      services_.editor_element_id ? services_.editor_element_id() : sl_element_id_t{0};
  std::vector<Photo> photos;
  for (const QVariant& value : targets) {
    const QVariantMap target = value.toMap();
    Photo             photo;
    photo.element_id =
        static_cast<sl_element_id_t>(target.value(QStringLiteral("elementId")).toULongLong());
    const auto image_id =
        static_cast<image_id_t>(target.value(QStringLiteral("imageId")).toULongLong());
    if (photo.element_id == 0) {
      continue;
    }
    photo.file_name = target.value(QStringLiteral("fileName")).toString();
    photo.path      = image_id != 0 && services_.image_path ? services_.image_path(image_id)
                                                            : std::filesystem::path{};
    photo.in_editor = editor_id != 0 && photo.element_id == editor_id;
    photos.push_back(std::move(photo));
  }
  if (photos.empty()) {
    return Refused(Tr("No valid image was selected."));
  }
  StartRun(QStringLiteral("turn_on"), std::move(photos), DevelopAiDenoise{std::move(version)},
           true);
  return QVariantMap{{QStringLiteral("started"), true},
                     {QStringLiteral("result"), QStringLiteral("started")},
                     {QStringLiteral("message"), QString()}};
}

auto AiDenoiseLibraryActions::turnOffForExportItem(uint elementId) -> QVariantMap {
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
  Photo photo;
  photo.element_id = element_id;
  if (export_queue_ != nullptr) {
    for (const auto& entry : export_queue_->Entries()) {
      if (entry.element_id == element_id) {
        photo.file_name = entry.file_name;
      }
    }
  }
  photo.in_editor = services_.editor_element_id && services_.editor_element_id() == element_id;
  std::vector<Photo> photos;
  photos.push_back(std::move(photo));
  StartRun(QStringLiteral("turn_off"), std::move(photos), std::nullopt, false);
  return QVariantMap{{QStringLiteral("started"), true},
                     {QStringLiteral("result"), QStringLiteral("started")},
                     {QStringLiteral("message"), QString()}};
}

void AiDenoiseLibraryActions::StartRun(const QString& action, std::vector<Photo> photos,
                                       std::optional<DevelopAiDenoise> denoise, bool check_sensor) {
  running_ = true;
  emit RunningChanged();
  if (background_tasks_ != nullptr) {
    // The run writes stored histories: no other owner may open, delete, or paste into them.
    const QString          reason = Tr("AI Denoise is being changed.");
    BackgroundTaskSnapshot snapshot;
    snapshot.kind_             = BackgroundTaskKind::AiDenoiseSwitch;
    snapshot.state_            = BackgroundTaskState::Running;
    snapshot.title_            = Tr("AI Denoise");
    snapshot.detail_           = Tr("%1 images").arg(static_cast<int>(photos.size()));
    snapshot.progress_percent_ = -1;
    snapshot.cancelable_       = false;
    snapshot.shutdown_policy_  = BackgroundTaskShutdownPolicy::WaitForFinish;
    for (const Photo& photo : photos) {
      snapshot.affected_targets_.push_back(static_cast<quint64>(photo.element_id));
    }
    snapshot.locks_ = {
        {InteractionCapability::PasteAdjustments, 0, reason},
        {InteractionCapability::CloseProject, 0, reason},
        {InteractionCapability::DeleteImages, 0, reason},
        {InteractionCapability::SelectEditorImage, 0, reason},
        {InteractionCapability::SwitchWorkspace, 0, reason},
    };
    task_id_ = background_tasks_->RegisterTask(snapshot);
  }

  QPointer<AiDenoiseLibraryActions> self(this);
  auto                shared_photos = std::make_shared<const std::vector<Photo>>(std::move(photos));
  StoredHistoryWriter write =
      services_.stored_history_writer ? services_.stored_history_writer() : StoredHistoryWriter{};
  QThreadPool::globalInstance()->start([self, read_sensor_layout = services_.read_sensor_layout,
                                        write   = std::move(write), action, shared_photos,
                                        denoise = std::move(denoise), check_sensor]() {
    std::vector<PhotoResult> results;
    results.reserve(shared_photos->size());
    for (const Photo& photo : *shared_photos) {
      results.push_back(RunPhoto(read_sensor_layout, write, photo, denoise, check_sensor));
    }
    QMetaObject::invokeMethod(
        self.data(),
        [self, action, shared_photos, denoise, results = std::move(results)]() mutable {
          if (self) {
            self->FinishRun(action, *shared_photos, std::move(results), denoise);
          }
        },
        Qt::QueuedConnection);
  });
}

auto AiDenoiseLibraryActions::RunPhoto(
    const std::function<RawSensorLayout(const std::filesystem::path&)>& read_sensor_layout,
    const StoredHistoryWriter& write, const Photo& photo,
    const std::optional<DevelopAiDenoise>& denoise, bool check_sensor) -> PhotoResult {
  PhotoResult result;
  if (check_sensor) {
    // D14: Bayer RAW photos only. A raster image and a file that LibRaw cannot open
    // (ReadRawSensorLayout throws) are not Bayer RAW photos.
    RawSensorLayout layout = RawSensorLayout::Other;
    if (!photo.path.empty() && read_sensor_layout) {
      try {
        layout = read_sensor_layout(photo.path);
      } catch (const std::exception& error) {
        ALCEDO_LOG_WARN(Render) << "AI Denoise: the sensor of photo " << photo.element_id
                                << " is unknown: " << error.what();
      }
    }
    if (layout != RawSensorLayout::Bayer) {
      result.outcome = PhotoOutcome::NotSupported;
      result.message = layout == RawSensorLayout::XTrans ? Tr("Not supported for X-Trans sensors")
                                                         : Tr("Not supported for this sensor");
      return result;
    }
  }
  if (photo.in_editor) {
    result.outcome = PhotoOutcome::ForEditor;
    return result;
  }
  if (!write) {
    result.outcome = PhotoOutcome::Failed;
    result.message = Tr("Pipeline service is unavailable.");
    return result;
  }
  try {
    result.write   = write(photo.element_id, denoise);
    result.outcome = result.write.changed ? PhotoOutcome::Written : PhotoOutcome::Unchanged;
  } catch (const std::exception& error) {
    result.outcome = PhotoOutcome::Failed;
    result.message = QString::fromStdString(error.what());
    ALCEDO_LOG_WARN(Render) << error.what();
  }
  return result;
}

void AiDenoiseLibraryActions::FinishRun(const QString& action, const std::vector<Photo>& photos,
                                        std::vector<PhotoResult>               results,
                                        const std::optional<DevelopAiDenoise>& denoise) {
  int          done = 0;
  QVariantList not_supported;
  QVariantList failures;
  for (std::size_t index = 0; index < photos.size(); ++index) {
    const Photo& photo  = photos[index];
    PhotoResult& result = results[index];
    if (result.outcome == PhotoOutcome::ForEditor) {
      const bool accepted = services_.write_editor_photo && services_.write_editor_photo(denoise);
      result.outcome      = accepted ? PhotoOutcome::Written : PhotoOutcome::Failed;
      if (!accepted) {
        result.message = Tr("The editor did not accept the change.");
      }
    } else if (result.outcome == PhotoOutcome::Written && services_.refresh_written_photo) {
      services_.refresh_written_photo(photo.element_id, result.write);
    }
    switch (result.outcome) {
      case PhotoOutcome::Written:
      case PhotoOutcome::Unchanged:
        ++done;
        if (action == QStringLiteral("turn_off") && export_queue_ != nullptr) {
          export_queue_->ReturnToQueued(photo.element_id);
        }
        break;
      case PhotoOutcome::NotSupported:
        not_supported.push_back(PhotoRow(photo.element_id, photo.file_name, result.message));
        break;
      case PhotoOutcome::Failed:
      case PhotoOutcome::ForEditor:
        failures.push_back(PhotoRow(photo.element_id, photo.file_name, result.message));
        break;
    }
  }

  const bool success = failures.isEmpty() && not_supported.isEmpty();
  QString    message;
  if (action == QStringLiteral("turn_on")) {
    message =
        TurnOnMessage(done, static_cast<int>(photos.size()), static_cast<int>(not_supported.size()),
                      static_cast<int>(failures.size()));
  } else if (!failures.isEmpty()) {
    message = Tr("AI Denoise could not be turned off: %1")
                  .arg(failures.front().toMap().value(QStringLiteral("message")).toString());
  }
  const QVariantMap result{{QStringLiteral("action"), action},
                           {QStringLiteral("success"), success},
                           {QStringLiteral("message"), message},
                           {QStringLiteral("doneCount"), done},
                           {QStringLiteral("notSupported"), not_supported},
                           {QStringLiteral("failures"), failures}};
  if (background_tasks_ != nullptr && !task_id_.isEmpty()) {
    background_tasks_->FinishTask(
        task_id_, success ? BackgroundTaskState::Succeeded : BackgroundTaskState::Failed, message);
  }
  task_id_.clear();
  running_ = false;
  emit RunningChanged();
  emit RunFinished(result);
  emit runFinished(result);
}

}  // namespace alcedo::ui
