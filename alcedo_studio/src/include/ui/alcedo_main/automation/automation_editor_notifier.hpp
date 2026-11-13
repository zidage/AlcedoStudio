//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QJsonObject>
#include <QObject>
#include <QPointer>

#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"

namespace alcedo::automation {

/// Sends the editor changes of the session as the `session.state_changed` and `history.changed`
/// notifications (plan section 2.5). GUI thread only.
///
/// It observes the EditorSessionController and reads the values through its API. A change made
/// by any actor (an automation command, or a user in the GUI host) is reported, because the
/// controller publishes all of them.
///
/// - `session.state_changed` has `state` (EditorSessionStateName), `element_id`, and
///   `image_id`. It is sent when one of them changes. StateChanged of the controller also fires
///   for render progress and other values; those do not send a notification.
/// - `history.changed` has `element_id`, `image_id`, `head_commit` (null without a head), and
///   `version_id` (the active Version). It is sent when one of them changes after HistoryChanged
///   of the controller, which fires when the history revision of the session advances.
class AutomationEditorNotifier final : public QObject {
  Q_OBJECT

 public:
  /// Reads the current values of @p editor, so that the first notification reports a change.
  explicit AutomationEditorNotifier(ui::EditorSessionController* editor, QObject* parent = nullptr);

  /// The values of the last `session.state_changed`, or the values at construction.
  [[nodiscard]] auto session_state() const -> const QJsonObject& { return session_state_; }
  /// The values of the last `history.changed`, or the values at construction.
  [[nodiscard]] auto history() const -> const QJsonObject& { return history_; }

 signals:
  void SessionStateChanged(const QJsonObject& params);
  void HistoryChanged(const QJsonObject& params);

 private:
  void                                  OnStateChanged();
  void                                  OnHistoryChanged();
  [[nodiscard]] auto                    ReadSessionState() const -> QJsonObject;
  [[nodiscard]] auto                    ReadHistory() const -> QJsonObject;

  QPointer<ui::EditorSessionController> editor_;
  QJsonObject                           session_state_;
  QJsonObject                           history_;
};

}  // namespace alcedo::automation
