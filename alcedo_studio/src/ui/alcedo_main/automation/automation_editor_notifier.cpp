//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/automation/automation_editor_notifier.hpp"

#include <QJsonValue>
#include <QString>
#include <utility>

#include "app/editor_history_types.hpp"
#include "app/editor_session_types.hpp"

namespace alcedo::automation {

AutomationEditorNotifier::AutomationEditorNotifier(ui::EditorSessionController* editor,
                                                   QObject*                     parent)
    : QObject(parent), editor_(editor) {
  if (editor_ == nullptr) {
    return;
  }
  session_state_ = ReadSessionState();
  history_       = ReadHistory();
  connect(editor_, &ui::EditorSessionController::StateChanged, this,
          &AutomationEditorNotifier::OnStateChanged);
  connect(editor_, &ui::EditorSessionController::HistoryChanged, this,
          &AutomationEditorNotifier::OnHistoryChanged);
}

void AutomationEditorNotifier::OnStateChanged() {
  QJsonObject state = ReadSessionState();
  if (state == session_state_) {
    return;
  }
  session_state_ = std::move(state);
  emit SessionStateChanged(session_state_);
}

void AutomationEditorNotifier::OnHistoryChanged() {
  QJsonObject history = ReadHistory();
  if (history == history_) {
    return;
  }
  history_ = std::move(history);
  emit HistoryChanged(history_);
}

auto AutomationEditorNotifier::ReadSessionState() const -> QJsonObject {
  return QJsonObject{
      {"state", QString::fromLatin1(EditorSessionStateName(editor_->session_state()))},
      {"element_id", static_cast<double>(editor_->element_id())},
      {"image_id", static_cast<double>(editor_->image_id())}};
}

auto AutomationEditorNotifier::ReadHistory() const -> QJsonObject {
  const EditorHistorySnapshot history = editor_->history_snapshot();
  const QJsonValue            head    = history.active_head.has_value()
                                            ? QJsonValue(QString::fromStdString(history.active_head->ToString()))
                                            : QJsonValue(QJsonValue::Null);
  return QJsonObject{{"element_id", static_cast<double>(editor_->element_id())},
                     {"image_id", static_cast<double>(editor_->image_id())},
                     {"head_commit", head},
                     {"version_id", QString::fromStdString(history.active_version_id.ToString())}};
}

}  // namespace alcedo::automation
