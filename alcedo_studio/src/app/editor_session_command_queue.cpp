//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_session_command_queue.hpp"

#include <utility>

namespace alcedo {

EditorSessionManualCommandExecutor::EditorSessionManualCommandExecutor()
    : owner_thread_(std::this_thread::get_id()) {}

void EditorSessionManualCommandExecutor::Post(std::function<void()> task) {
  if (!task) {
    return;
  }
  std::scoped_lock lock(mutex_);
  pending_.push(std::move(task));
}

auto EditorSessionManualCommandExecutor::IsOwnerThread() const -> bool {
  return std::this_thread::get_id() == owner_thread_;
}

auto EditorSessionManualCommandExecutor::DrainOne() -> bool {
  std::function<void()> task;
  {
    std::scoped_lock lock(mutex_);
    if (pending_.empty()) {
      return false;
    }
    task = std::move(pending_.front());
    pending_.pop();
  }
  if (task) {
    task();
  }
  return true;
}

void EditorSessionManualCommandExecutor::DrainAll() {
  while (DrainOne()) {
  }
}

auto EditorSessionManualCommandExecutor::pending() const -> std::size_t {
  std::scoped_lock lock(mutex_);
  return pending_.size();
}

struct EditorSessionCommandQueue::SharedState {
  mutable std::mutex      mutex;
  std::queue<Task>        pending;
  EditorSessionQueueState state           = EditorSessionQueueState::Accepting;
  std::uint64_t           next_command_id = 1;
  bool                    draining        = false;
};

EditorSessionCommandQueue::EditorSessionCommandQueue(
    std::shared_ptr<IEditorSessionCommandExecutor> executor)
    : executor_(executor ? std::move(executor)
                         : std::make_shared<EditorSessionManualCommandExecutor>()),
      state_(std::make_shared<SharedState>()) {}

EditorSessionCommandQueue::~EditorSessionCommandQueue() { Stop(); }

auto EditorSessionCommandQueue::Submit(EditorSessionCommand command, CommandHandler handler)
    -> Submission {
  Submission submission;
  if (!handler) {
    return submission;
  }

  const auto state = state_;
  {
    std::scoped_lock lock(state->mutex);
    if (state->state != EditorSessionQueueState::Accepting) {
      return submission;
    }
    command.operation.command_id = state->next_command_id++;
    submission.operation         = command.operation;
    submission.accepted          = true;
  }

  Task task = [state, handler = std::move(handler), command = std::move(command)]() mutable {
    {
      std::scoped_lock lock(state->mutex);
      if (state->state == EditorSessionQueueState::Stopped) {
        return;
      }
    }
    handler(std::move(command));
  };

  if (executor_->IsOwnerThread()) {
    EnqueueAndDrain(state, std::move(task));
    submission.executed = true;
  } else {
    executor_->Post(
        [state, task = std::move(task)]() mutable { EnqueueAndDrain(state, std::move(task)); });
  }
  return submission;
}

void EditorSessionCommandQueue::PostCompletion(Task task) {
  if (!task) {
    return;
  }
  const auto state    = state_;
  const auto executor = executor_;
  executor->Post(
      [state, task = std::move(task)]() mutable { EnqueueAndDrain(state, std::move(task)); });
}

void EditorSessionCommandQueue::BeginShutdown() {
  std::scoped_lock lock(state_->mutex);
  if (state_->state == EditorSessionQueueState::Accepting) {
    state_->state = EditorSessionQueueState::ShuttingDown;
  }
}

void EditorSessionCommandQueue::Stop() {
  std::scoped_lock lock(state_->mutex);
  state_->state = EditorSessionQueueState::Stopped;
  while (!state_->pending.empty()) {
    state_->pending.pop();
  }
}

auto EditorSessionCommandQueue::state() const -> EditorSessionQueueState {
  std::scoped_lock lock(state_->mutex);
  return state_->state;
}

auto EditorSessionCommandQueue::pending() const -> std::size_t {
  std::scoped_lock lock(state_->mutex);
  return state_->pending.size();
}

auto EditorSessionCommandQueue::IsOwnerThread() const -> bool { return executor_->IsOwnerThread(); }

void EditorSessionCommandQueue::EnqueueAndDrain(const std::shared_ptr<SharedState>& state,
                                                Task                                task) {
  if (!task) {
    return;
  }

  {
    std::scoped_lock lock(state->mutex);
    if (state->state == EditorSessionQueueState::Stopped) {
      return;
    }
    state->pending.push(std::move(task));
    if (state->draining) {
      return;
    }
    state->draining = true;
  }

  for (;;) {
    Task next;
    {
      std::scoped_lock lock(state->mutex);
      if (state->pending.empty()) {
        state->draining = false;
        return;
      }
      next = std::move(state->pending.front());
      state->pending.pop();
    }
    next();
  }
}

}  // namespace alcedo
