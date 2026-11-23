//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "app/ai_denoise_history_write.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/input/raw_sensor_layout.hpp"
#include "type/type.hpp"
#include "ui/alcedo_main/album_backend/background_task_controller.hpp"
#include "ui/alcedo_main/album_backend/export_queue.hpp"

namespace alcedo::ui {

/**
 * @brief AI Denoise outside the editor (integration plan, DN8): the `AI Denoise` action of the
 *        image context menu and `Turn Off AI Denoise` of an export queue item.
 *
 * Both write the switch as one `raw_decode` change on the active Version of a photo: into its
 * stored history on a worker thread (WriteAiDenoiseSwitchToStoredHistory), or through the editor
 * session when the editor has the photo open, because the session owns that history. Neither
 * runs the model. The action checks the sensor of each target first (D14): a photo that is not
 * a Bayer RAW file is reported and not written. One run at a time; a run holds a background task
 * with the locks of a Paste, because it writes stored histories.
 *
 * Thread: the GUI thread. The services run on the GUI thread, except `read_sensor_layout` and
 * the StoredHistoryWriter, which run on a worker thread.
 */
class AiDenoiseLibraryActions final : public QObject {
  Q_OBJECT
  /// True when the platform can run the model (D13); the menu item is absent otherwise.
  Q_PROPERTY(bool available READ available CONSTANT)
  Q_PROPERTY(bool running READ running NOTIFY RunningChanged)

 public:
  /// Writes the switch of one photo into its stored history; runs on a worker thread.
  using StoredHistoryWriter =
      std::function<AiDenoiseHistoryWrite(sl_element_id_t, const std::optional<DevelopAiDenoise>&)>;

  struct Services {
    /// DenoiseModelSession::IsPlatformAvailable.
    std::function<bool()>                                              platform_available;
    /// The installed model version; throws AiDenoiseError (ModelNotInstalled) without a model.
    std::function<std::string()>                                       installed_model_version;
    /// The refusal text of a user change while an automation agent holds control; empty when
    /// the user may change the library.
    std::function<QString()>                                           user_refusal;
    /// The file of an image of the project; empty when it is unknown.
    std::function<std::filesystem::path(image_id_t)>                   image_path;
    /// ReadRawSensorLayout in the product; runs on a worker thread.
    std::function<RawSensorLayout(const std::filesystem::path&)>       read_sensor_layout;
    /// The writer of a run, read on the GUI thread when the run starts:
    /// WriteAiDenoiseSwitchToStoredHistory bound to the pipeline service of the open project.
    /// Empty without a project; each photo of the run then fails.
    std::function<StoredHistoryWriter()>                               stored_history_writer;
    /// The element that the editor session has open; 0 when none.
    std::function<sl_element_id_t()>                                   editor_element_id;
    /// The switch of the open editor photo through the editor session (EditorAiDenoiseControl).
    /// False when the session refused the change.
    std::function<bool(const std::optional<DevelopAiDenoise>&)>        write_editor_photo;
    /// After a stored-history write that changed the photo: its edit state and thumbnail in the
    /// library.
    std::function<void(sl_element_id_t, const AiDenoiseHistoryWrite&)> refresh_written_photo;
  };

  /// @param background_tasks Optional; it shows the run and holds its locks.
  /// @param export_queue Optional; `Turn Off AI Denoise` returns its item to Queued.
  AiDenoiseLibraryActions(Services services, BackgroundTaskController* background_tasks,
                          ExportQueue* export_queue, QObject* parent = nullptr);

  /**
   * @brief Turn AI Denoise on for @p targets (maps with elementId, imageId, and fileName, the
   *        targets of LibraryMutationOperations::ResolveTargets).
   * @return `{started, result, message}`. `result` is `started`; `model_not_installed` (the
   *         caller opens the AI Models page; nothing is written); or `refused` with the reason
   *         in `message` (no platform, an agent holds control, no target, a run is running).
   *         The result of a started run arrives with RunFinished.
   */
  Q_INVOKABLE QVariantMap turnOnForTargets(const QVariantList& targets);

  /**
   * @brief `Turn Off AI Denoise` of the export queue item of @p elementId: the switch off on the
   *        active Version of the photo, then ExportQueue::ReturnToQueued.
   * @return `{started, result, message}` as turnOnForTargets (`started` or `refused`). The
   *         result arrives with RunFinished; a failed write keeps the item in its run.
   */
  Q_INVOKABLE QVariantMap turnOffForExportItem(uint elementId);

  [[nodiscard]] auto      available() const -> bool;
  [[nodiscard]] bool      running() const { return running_; }

 signals:
  void RunningChanged();
  /**
   * @brief A started run ended. @p result: `action` (`turn_on`, `turn_off`), `success`,
   *        `message` (for the user; empty after a turn-off that succeeded), `doneCount` (the photos
   *        whose switch has the requested value now),
   *        `notSupported` and `failures` (lists of `{elementId, fileName, message}`).
   */
  void RunFinished(const QVariantMap& result);
  // QML Connections maps a handler onFoo to the signal foo, so a signal that starts with an
  // uppercase letter needs a lowercase twin for QML.
  void runFinished(const QVariantMap& result);

 private:
  /// One photo of a run, as the GUI thread resolved it.
  struct Photo {
    sl_element_id_t       element_id = 0;
    QString               file_name;
    std::filesystem::path path;
    /// The editor has it open: the worker only checks its sensor, the GUI thread writes it.
    bool                  in_editor = false;
  };

  /// What the worker did with one photo.
  enum class PhotoOutcome { Written, Unchanged, NotSupported, Failed, ForEditor };

  struct PhotoResult {
    PhotoOutcome          outcome = PhotoOutcome::Failed;
    AiDenoiseHistoryWrite write;
    QString               message;
  };

  [[nodiscard]] auto        Refused(const QString& message) const -> QVariantMap;
  /// Starts the background task and the worker for @p photos.
  void                      StartRun(const QString& action, std::vector<Photo> photos,
                                     std::optional<DevelopAiDenoise> denoise, bool check_sensor);
  /// The worker part of one photo.
  [[nodiscard]] static auto RunPhoto(
      const std::function<RawSensorLayout(const std::filesystem::path&)>& read_sensor_layout,
      const StoredHistoryWriter& write, const Photo& photo,
      const std::optional<DevelopAiDenoise>& denoise, bool check_sensor) -> PhotoResult;
  /// GUI thread: the editor writes, the library refresh, the queue state, the result.
  void     FinishRun(const QString& action, const std::vector<Photo>& photos,
                     std::vector<PhotoResult> results, const std::optional<DevelopAiDenoise>& denoise);

  Services services_;
  QPointer<BackgroundTaskController> background_tasks_;
  QPointer<ExportQueue>              export_queue_;
  mutable std::optional<bool>        available_;
  bool                               running_ = false;
  QString                            task_id_;
};

}  // namespace alcedo::ui
