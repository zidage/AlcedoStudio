//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantMap>
#include <functional>
#include <optional>

#include "app/ai_denoise_history_write.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "type/type.hpp"
#include "ui/alcedo_main/album_backend/background_task_controller.hpp"
#include "ui/alcedo_main/album_backend/export_queue.hpp"

namespace alcedo::ui {

/**
 * @brief `Turn Off AI Denoise` of an export queue item whose render failed with an AI Denoise
 *        error (integration plan, DN8).
 *
 * Writes the switch off as one `raw_decode` change on the active Version of the photo: into its
 * stored history on a worker thread (WriteAiDenoiseSwitchToStoredHistory), or through the editor
 * session when the editor has the photo open, because the session owns that history. Then it
 * returns the item to Queued (ExportQueue::ReturnToQueued); it starts no export. One write at a
 * time; a write holds a background task with the locks of a Paste.
 *
 * Thread: the GUI thread. The services run on the GUI thread, except the StoredHistoryWriter,
 * which runs on a worker thread.
 */
class ExportAiDenoiseControl final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool running READ running NOTIFY RunningChanged)

 public:
  /// Writes the switch of one photo into its stored history; runs on a worker thread.
  using StoredHistoryWriter =
      std::function<AiDenoiseHistoryWrite(sl_element_id_t, const std::optional<DevelopAiDenoise>&)>;

  struct Services {
    /// The refusal text of a user change while an automation agent holds control; empty when
    /// the user may change the library.
    std::function<QString()>                                           user_refusal;
    /// The writer of a run, read on the GUI thread when the run starts:
    /// WriteAiDenoiseSwitchToStoredHistory bound to the pipeline service of the open project.
    /// Empty without a project; the write then fails.
    std::function<StoredHistoryWriter()>                               stored_history_writer;
    /// The element that the editor session has open; 0 when none.
    std::function<sl_element_id_t()>                                   editor_element_id;
    /// The switch off for the open editor photo through the editor session
    /// (EditorAiDenoiseControl::turnOff). False when the session refused the change.
    std::function<bool()>                                              turn_off_editor_photo;
    /// After a stored-history write that changed the photo: its edit state and thumbnail in the
    /// library.
    std::function<void(sl_element_id_t, const AiDenoiseHistoryWrite&)> refresh_written_photo;
  };

  /// @param background_tasks Optional; it shows the write and holds its locks.
  /// @param export_queue The queue whose item returns to Queued; optional for tests.
  ExportAiDenoiseControl(Services services, BackgroundTaskController* background_tasks,
                         ExportQueue* export_queue, QObject* parent = nullptr);

  /**
   * @brief `Turn Off AI Denoise` of the export queue item of @p elementId.
   * @return `{started, message}`: false with the reason when an agent holds control, a write
   *         runs, or the id is 0. The result of a started write arrives with TurnOffFinished; a
   *         failed write keeps the item in its run.
   */
  Q_INVOKABLE QVariantMap turnOffForExportItem(uint elementId);

  [[nodiscard]] bool      running() const { return running_; }

 signals:
  void RunningChanged();
  /// A started write ended. @p result: `elementId`, `success`, and `message` (the error for the
  /// user; empty after a write that succeeded).
  void TurnOffFinished(const QVariantMap& result);
  // QML Connections maps a handler onFoo to the signal foo, so a signal that starts with an
  // uppercase letter needs a lowercase twin for QML.
  void turnOffFinished(const QVariantMap& result);

 private:
  [[nodiscard]] static auto Refused(const QString& message) -> QVariantMap;
  /// GUI thread: the library refresh, the queue state, the result.
  void     Finish(sl_element_id_t element_id, std::optional<AiDenoiseHistoryWrite> write,
                  const QString& error);

  Services services_;
  QPointer<BackgroundTaskController> background_tasks_;
  QPointer<ExportQueue>              export_queue_;
  bool                               running_ = false;
  QString                            task_id_;
};

}  // namespace alcedo::ui
