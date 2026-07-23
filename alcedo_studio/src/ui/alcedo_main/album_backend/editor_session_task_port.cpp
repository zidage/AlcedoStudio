//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_session_task_port.hpp"

#include <QMetaObject>
#include <QObject>
#include <QThread>
#include <QVariantList>
#include <utility>

#include "ui/alcedo_main/album_backend/background_task_controller.hpp"

namespace alcedo::ui {

EditorSessionTaskPort::EditorSessionTaskPort(BackgroundTaskController* background_tasks)
    : background_tasks_(background_tasks) {}

void EditorSessionTaskPort::SetBackgroundTasks(BackgroundTaskController* background_tasks) {
  std::scoped_lock lock(mutex_);
  background_tasks_ = background_tasks;
}

auto EditorSessionTaskPort::BeginTask(const std::string& name, sl_element_id_t element_id)
    -> std::uint64_t {
  std::uint64_t             task_id = 0;
  BackgroundTaskController* tasks   = nullptr;
  {
    std::scoped_lock lock(mutex_);
    task_id = ++next_id_;
    tasks   = background_tasks_;
  }
  if (!tasks) {
    return task_id;
  }

  BackgroundTaskSnapshot snapshot;
  snapshot.kind_             = BackgroundTaskKind::EditorSave;
  snapshot.state_            = BackgroundTaskState::Running;
  snapshot.title_            = QString::fromUtf8(name.empty() ? "editor_save" : name.c_str());
  snapshot.detail_           = QObject::tr("Saving editor changes");
  snapshot.progress_percent_ = -1;
  snapshot.cancelable_       = false;
  snapshot.shutdown_policy_  = BackgroundTaskShutdownPolicy::WaitForFinish;
  if (element_id != 0) {
    snapshot.affected_targets_ = QVariantList{static_cast<qulonglong>(element_id)};
  }
  const QString save_reason = QObject::tr("Saving editor changes");
  snapshot.locks_           = {
      {InteractionCapability::SelectEditorImage, 0, save_reason},
      {InteractionCapability::SwitchWorkspace, 0, save_reason},
      {InteractionCapability::CheckoutVersion, 0, save_reason},
      {InteractionCapability::PasteAdjustments, 0, save_reason},
      {InteractionCapability::MergeAdjustments, 0, save_reason},
  };
  const QString ui_id = tasks->RegisterTask(snapshot, {});
  {
    std::scoped_lock lock(mutex_);
    active_task_ids_[task_id] = ui_id;
  }
  return task_id;
}

void EditorSessionTaskPort::EndTask(std::uint64_t task_id, bool success,
                                    const std::string& message) {
  QString                   ui_id;
  BackgroundTaskController* tasks = nullptr;
  {
    std::scoped_lock lock(mutex_);
    tasks   = background_tasks_;
    auto it = active_task_ids_.find(task_id);
    if (it != active_task_ids_.end()) {
      ui_id = it->second;
      active_task_ids_.erase(it);
    }
  }
  if (!tasks || ui_id.isEmpty()) {
    return;
  }
  const auto final_state = success ? BackgroundTaskState::Succeeded : BackgroundTaskState::Failed;
  const auto detail      = QString::fromUtf8(message.c_str());
  auto       finish      = [tasks, ui_id, final_state, detail] {
    tasks->FinishTask(ui_id, final_state, detail);
  };
  if (QThread::currentThread() == tasks->thread()) {
    finish();
  } else {
    QMetaObject::invokeMethod(tasks, std::move(finish), Qt::QueuedConnection);
  }
}

}  // namespace alcedo::ui
