//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_session_command_queue.hpp"

#include <algorithm>
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

void EditorSessionManualCommandExecutor::PostDelayed(std::function<void()>    task,
                                                     std::chrono::nanoseconds /*delay*/) {
  if (!task) {
    return;
  }
  std::scoped_lock lock(mutex_);
  delayed_.push(std::move(task));
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
  // Drain delayed completions too: tests advance no clock, so a posted pacing
  // deadline must be runnable through the same explicit drain as immediate work.
  while (DrainOne() || DrainDelayedOne()) {
  }
}

auto EditorSessionManualCommandExecutor::DrainDelayedOne() -> bool {
  std::function<void()> task;
  {
    std::scoped_lock lock(mutex_);
    if (delayed_.empty()) {
      return false;
    }
    task = std::move(delayed_.front());
    delayed_.pop();
  }
  if (task) {
    task();
  }
  return true;
}

void EditorSessionManualCommandExecutor::DrainDelayedAll() {
  while (DrainDelayedOne()) {
  }
}

auto EditorSessionManualCommandExecutor::pending() const -> std::size_t {
  std::scoped_lock lock(mutex_);
  return pending_.size();
}

auto EditorSessionManualCommandExecutor::pending_delayed() const -> std::size_t {
  std::scoped_lock lock(mutex_);
  return delayed_.size();
}

EditorSessionThreadedCommandExecutor::EditorSessionThreadedCommandExecutor()
    : thread_([this] { Run(); }) {}

EditorSessionThreadedCommandExecutor::~EditorSessionThreadedCommandExecutor() { Shutdown(); }

void EditorSessionThreadedCommandExecutor::Shutdown() {
  Stop();
  if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) {
    thread_.join();
  }
}

void EditorSessionThreadedCommandExecutor::Post(std::function<void()> task) {
  PostDelayed(std::move(task), std::chrono::nanoseconds::zero());
}

void EditorSessionThreadedCommandExecutor::PostDelayed(std::function<void()>    task,
                                                       std::chrono::nanoseconds delay) {
  if (!task) {
    return;
  }
  ScheduledTask scheduled;
  scheduled.deadline = std::chrono::steady_clock::now() +
                       std::max(delay, std::chrono::nanoseconds::zero());
  scheduled.task = std::move(task);
  {
    std::scoped_lock lock(mutex_);
    ScheduleLocked(std::move(scheduled));
  }
  cv_.notify_one();
}

auto EditorSessionThreadedCommandExecutor::IsOwnerThread() const -> bool {
  return std::this_thread::get_id() == owner_thread_.load(std::memory_order_acquire);
}

void EditorSessionThreadedCommandExecutor::Stop() {
  {
    std::scoped_lock lock(mutex_);
    stopping_ = true;
  }
  cv_.notify_one();
}

void EditorSessionThreadedCommandExecutor::ScheduleLocked(ScheduledTask scheduled) {
  scheduled.sequence = next_sequence_++;
  scheduled_.push_back(std::move(scheduled));
  std::push_heap(scheduled_.begin(), scheduled_.end(),
                 [](const ScheduledTask& a, const ScheduledTask& b) {
                   return std::tie(a.deadline, a.sequence) > std::tie(b.deadline, b.sequence);
                 });
}

void EditorSessionThreadedCommandExecutor::Run() {
  owner_thread_.store(std::this_thread::get_id(), std::memory_order_release);
  std::unique_lock lock(mutex_);
  for (;;) {
    // Run every task whose deadline has elapsed. New posts arriving while a
    // task runs are picked up on the next pass of this loop.
    while (!scheduled_.empty() &&
           scheduled_.front().deadline <= std::chrono::steady_clock::now()) {
      std::pop_heap(scheduled_.begin(), scheduled_.end(),
                    [](const ScheduledTask& a, const ScheduledTask& b) {
                      return std::tie(a.deadline, a.sequence) >
                             std::tie(b.deadline, b.sequence);
                    });
      auto task = std::move(scheduled_.back().task);
      scheduled_.pop_back();
      lock.unlock();
      task();
      lock.lock();
    }
    if (stopping_) {
      return;
    }
    if (scheduled_.empty()) {
      cv_.wait(lock);
    } else {
      cv_.wait_until(lock, scheduled_.front().deadline);
    }
  }
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

void EditorSessionCommandQueue::PostCompletionDelayed(Task task,
                                                      std::chrono::nanoseconds delay) {
  if (!task) {
    return;
  }
  const auto state    = state_;
  const auto executor = executor_;
  executor->PostDelayed(
      [state, task = std::move(task)]() mutable { EnqueueAndDrain(state, std::move(task)); },
      delay);
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

  // Drain nested owner-thread Submit work only. Independently posted
  // completions and consume wakeups enter through the executor, so Qt can
  // still process pointer and window-update events between those turns.
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
