//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/agent_control_owner.hpp"

#include <QMetaObject>
#include <QPointer>
#include <algorithm>
#include <utility>

#include "ui/alcedo_main/i18n.hpp"

namespace alcedo::ui {

AgentControlOwner::AgentControlOwner(BackgroundTaskController* tasks, QObject* parent)
    : QObject(parent), tasks_(tasks) {}

void AgentControlOwner::SetUnsealedInputReader(UnsealedInputReader reader) {
  unsealed_input_reader_ = std::move(reader);
}

auto AgentControlOwner::HasUnsealedInput() const -> bool {
  return unsealed_input_reader_ && unsealed_input_reader_();
}

auto AgentControlOwner::CancelCallback() -> std::function<void()> {
  // The task registry calls the cancel callback inside CancelTask, before CancelTask returns.
  // StopByUser changes the control state at once and finishes the task after CancelTask.
  QPointer<AgentControlOwner> self = this;
  return [self]() {
    if (self != nullptr) {
      self->StopByUser();
    }
  };
}

auto AgentControlOwner::TryAcquire(const QString& holder_name) -> AgentControlRefusal {
  if (held_) {
    return holder_name == holder_name_ ? AgentControlRefusal::None
                                       : AgentControlRefusal::HeldByOtherAgent;
  }
  if (HasUnsealedInput()) {
    return AgentControlRefusal::UnsealedEditorInput;
  }

  held_        = true;
  holder_name_ = holder_name;
  // The user told the agent to continue: the stopped session continues in its own task. The
  // user can have dismissed that task, or another holder acquires; then a new task starts.
  const bool resumed = stopped_by_user_ && holder_name == stopped_holder_name_ &&
                       tasks_->ResumeTask(task_id_, WaitingDetail(), CancelCallback());
  stopped_by_user_ = false;
  stopped_holder_name_.clear();
  if (resumed) {
    (void)tasks_->AppendTaskActivity(task_id_, Tr("The agent continued."), QString(),
                                     BackgroundTaskActivityState::Succeeded);
  } else {
    BackgroundTaskSnapshot snapshot;
    snapshot.kind_             = BackgroundTaskKind::AgentControl;
    snapshot.state_            = BackgroundTaskState::Running;
    snapshot.title_            = holder_name;
    snapshot.detail_           = WaitingDetail();
    snapshot.progress_percent_ = -1;
    snapshot.cancelable_       = true;
    snapshot.shutdown_policy_  = BackgroundTaskShutdownPolicy::CancelAndWait;
    snapshot.pinned_           = true;
    snapshot.locks_.push_back(InteractionLock{InteractionCapability::UserStateChange, 0,
                                              Tr("An agent controls the session.")});
    task_id_ = tasks_->RegisterTask(snapshot, CancelCallback());
    (void)tasks_->AppendTaskActivity(task_id_, Tr("The agent took control."), QString(),
                                     BackgroundTaskActivityState::Succeeded);
  }
  emit ControlChanged();
  return AgentControlRefusal::None;
}

void AgentControlOwner::RequestAcquire(const QString& holder_name, AcquireWaitCheck still_waiting,
                                       AcquireDone done) {
  const AgentControlRefusal refusal = TryAcquire(holder_name);
  if (refusal != AgentControlRefusal::UnsealedEditorInput) {
    done(refusal);
    return;
  }
  pending_acquires_.push_back(
      PendingAcquire{holder_name, std::move(still_waiting), std::move(done)});
}

void AgentControlOwner::RecheckPendingAcquires() {
  // A done callback can start another request, so the list is taken first.
  std::vector<PendingAcquire> pending = std::move(pending_acquires_);
  pending_acquires_.clear();
  for (auto& request : pending) {
    if (request.still_waiting_ && !request.still_waiting_()) {
      continue;
    }
    const AgentControlRefusal refusal = TryAcquire(request.holder_name_);
    if (refusal == AgentControlRefusal::UnsealedEditorInput) {
      pending_acquires_.push_back(std::move(request));
      continue;
    }
    request.done_(refusal);
  }
}

auto AgentControlOwner::Release() -> bool {
  if (!held_) {
    return false;
  }
  held_ = false;
  holder_name_.clear();
  (void)tasks_->AppendTaskActivity(task_id_, Tr("The agent gave control back to you."), QString(),
                                   BackgroundTaskActivityState::Succeeded);
  tasks_->FinishTask(task_id_, BackgroundTaskState::Succeeded, Tr("Control returned to you."));
  emit ControlChanged();
  return true;
}

void AgentControlOwner::Stop() {
  if (held_) {
    (void)tasks_->CancelTask(task_id_);
  }
}

void AgentControlOwner::StopByUser() {
  if (!held_) {
    return;
  }
  held_                = false;
  stopped_by_user_     = true;
  stopped_holder_name_ = holder_name_;
  holder_name_.clear();
  (void)tasks_->AppendTaskActivity(task_id_, Tr("You stopped the agent."), QString(),
                                   BackgroundTaskActivityState::Succeeded);
  // FinishTask drops the cancel callback that is running now, so it runs after CancelTask.
  const QString task_id = task_id_;
  QMetaObject::invokeMethod(
      this,
      [this, task_id]() {
        tasks_->FinishTask(task_id, BackgroundTaskState::Canceled,
                           Tr("Stopped. Tell the agent to continue."));
      },
      Qt::QueuedConnection);
  emit ControlChanged();
}

auto AgentControlOwner::BeginCommand(const QString& summary, const QString& note) -> quint64 {
  if (!held_) {
    return 0;
  }
  const quint64 token = ++next_command_token_;
  const quint64 activity_id =
      tasks_->AppendTaskActivity(task_id_, summary, note, BackgroundTaskActivityState::Running);
  running_commands_.push_back(RunningCommand{token, task_id_, activity_id, summary});
  PublishTaskDetail();
  return token;
}

void AgentControlOwner::EndCommand(quint64 token, bool succeeded) {
  const auto found =
      std::find_if(running_commands_.begin(), running_commands_.end(),
                   [token](const RunningCommand& command) { return command.token_ == token; });
  if (found == running_commands_.end()) {
    return;
  }
  tasks_->FinishTaskActivity(
      found->task_id_, found->activity_id_,
      succeeded ? BackgroundTaskActivityState::Succeeded : BackgroundTaskActivityState::Failed);
  running_commands_.erase(found);
  PublishTaskDetail();
}

auto AgentControlOwner::WaitingDetail() -> QString { return Tr("Waiting for the next command"); }

void AgentControlOwner::PublishTaskDetail() {
  if (!held_) {
    return;
  }
  QString detail = WaitingDetail();
  for (auto it = running_commands_.rbegin(); it != running_commands_.rend(); ++it) {
    if (it->task_id_ == task_id_) {
      detail = it->summary_;
      break;
    }
  }
  tasks_->UpdateTask(task_id_, holder_name_, detail, -1);
}

}  // namespace alcedo::ui
