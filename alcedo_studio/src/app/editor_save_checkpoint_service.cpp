//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_save_checkpoint_service.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <utility>

namespace alcedo {

// ── AsyncCallbackGate ────────────────────────────────────────────────────────

auto EditorSaveCheckpointService::AsyncCallbackGate::Enter() -> bool {
  std::scoped_lock lock(mutex);
  if (stopping) {
    return false;
  }
  ++active_callbacks;
  return true;
}

void EditorSaveCheckpointService::AsyncCallbackGate::Leave() {
  std::scoped_lock lock(mutex);
  if (active_callbacks > 0) {
    --active_callbacks;
  }
  if (stopping && active_callbacks == 0) {
    condition.notify_all();
  }
}

void EditorSaveCheckpointService::AsyncCallbackGate::StopAndWait() {
  std::unique_lock lock(mutex);
  stopping = true;
  condition.wait(lock, [this] { return active_callbacks == 0; });
}

// ── EditorSaveCheckpointService ──────────────────────────────────────────────

EditorSaveCheckpointService::EditorSaveCheckpointService(Dependencies dependencies)
    : deps_(std::move(dependencies)), callback_gate_(std::make_shared<AsyncCallbackGate>()) {}

EditorSaveCheckpointService::~EditorSaveCheckpointService() {
  if (callback_gate_) {
    callback_gate_->StopAndWait();
  }
}

auto EditorSaveCheckpointService::Start(const SaveCheckpointRequest& request,
                                        SaveCheckpointCompletion completion) -> CheckpointTicket {
  std::uint64_t task_id = 0;
  if (deps_.tasks) {
    task_id = deps_.tasks->BeginTask("editor_save", request.element_id);
    if (task_id == 0) {
      if (completion) {
        SaveCheckpointResult result;
        result.session_generation   = request.session_generation;
        result.checkpoint_completed = false;
        result.error                = "Failed to start editor save task";
        completion(result);
      }
      return CheckpointTicket{};
    }
  }

  const std::uint64_t request_id = [this] {
    std::scoped_lock lock(mutex_);
    return next_request_id_++;
  }();

  {
    std::scoped_lock lock(mutex_);
    pending_saves_.push_back(PendingSave{request_id, request.session_generation, request.element_id,
                                         task_id, completion});
  }

  struct StartObservation {
    std::atomic<bool> completed{false};
    std::atomic<bool> commit_succeeded{true};
    std::string       error;
  };
  const auto observation = std::make_shared<StartObservation>();
  const auto gate        = callback_gate_;
  const auto on_commit   = [this, gate, observation, request_id,
                          completion](EditorJournalCommitOutcome outcome) {
    observation->error = outcome.error;
    observation->commit_succeeded.store(outcome.accepted && outcome.durable,
                                          std::memory_order_release);
    observation->completed.store(true, std::memory_order_release);
    if (!gate || !gate->Enter()) {
      return;
    }
    HandleJournalCommit(request_id, std::move(outcome), completion);
    gate->Leave();
  };

  bool started_async = true;
  if (deps_.journal) {
    started_async = deps_.journal->CommitJournalAsync(request.element_id,
                                                      request.session_generation, on_commit);
  } else {
    on_commit(EditorJournalCommitOutcome{true, true, false, 0, 0, {}});
  }

  if (!started_async) {
    if (!observation->completed.load(std::memory_order_acquire)) {
      std::uint64_t            rolled_task_id = 0;
      SaveCheckpointCompletion rolled_completion;
      if (TakePendingSave(request_id, &rolled_task_id, &rolled_completion) && deps_.tasks &&
          rolled_task_id != 0) {
        deps_.tasks->EndTask(rolled_task_id, false, "Journal commit could not start");
      }
      if (rolled_completion) {
        SaveCheckpointResult result;
        result.request_id           = request_id;
        result.session_generation   = request.session_generation;
        result.task_id              = rolled_task_id;
        result.checkpoint_completed = false;
        result.error =
            observation->error.empty() ? "Journal commit could not start" : observation->error;
        rolled_completion(result);
      }
    }
    return CheckpointTicket{};
  }

  if (observation->completed.load(std::memory_order_acquire) &&
      !observation->commit_succeeded.load(std::memory_order_acquire)) {
    return CheckpointTicket{};
  }

  return CheckpointTicket{request_id, request.session_generation, request.element_id, task_id};
}

void EditorSaveCheckpointService::CancelAndWait() {
  if (callback_gate_) {
    callback_gate_->StopAndWait();
  }
}

auto EditorSaveCheckpointService::active() const -> bool {
  std::scoped_lock lock(mutex_);
  return !pending_saves_.empty();
}

void EditorSaveCheckpointService::OnCheckpointFinished(const SaveCheckpointResult& result) {
  SaveCheckpointCompletion completion;
  std::uint64_t            task_id = 0;
  {
    std::scoped_lock lock(mutex_);
    auto             it = std::find_if(pending_saves_.begin(), pending_saves_.end(),
                                       [&result](const PendingSave& save) {
                             return save.request_id == result.request_id &&
                                    save.session_generation == result.session_generation;
                           });
    if (it == pending_saves_.end()) {
      return;
    }
    task_id    = it->task_id;
    completion = it->completion;
    pending_saves_.erase(it);
  }
  if (deps_.tasks && task_id != 0) {
    deps_.tasks->EndTask(task_id, result.checkpoint_completed,
                         result.error.empty() ? "checkpoint finished" : result.error);
  }
  if (completion) {
    completion(result);
  }
}

void EditorSaveCheckpointService::HandleJournalCommit(std::uint64_t              request_id,
                                                      EditorJournalCommitOutcome outcome,
                                                      SaveCheckpointCompletion   completion) {
  std::uint64_t   task_id           = 0;
  sl_element_id_t element_id        = 0;
  std::uint64_t   session_gen       = 0;
  bool            found             = false;
  bool            start_materialize = false;
  {
    std::scoped_lock lock(mutex_);
    auto             it = std::find_if(
        pending_saves_.begin(), pending_saves_.end(),
        [request_id](const PendingSave& save) { return save.request_id == request_id; });
    if (it == pending_saves_.end()) {
      return;
    }
    found       = true;
    task_id     = it->task_id;
    element_id  = it->element_id;
    session_gen = it->session_generation;

    if (!outcome.accepted || !outcome.durable) {
      pending_saves_.erase(it);
    } else if (!deps_.journal) {
      pending_saves_.erase(it);
    } else {
      start_materialize = true;
    }
  }

  if (!found) {
    return;
  }

  if (start_materialize) {
    const auto gate    = callback_gate_;
    const bool started = deps_.journal->MaterializeAsync(
        element_id, session_gen,
        [this, gate, request_id, completion](EditorMaterializeOutcome materialized) mutable {
          if (!gate || !gate->Enter()) {
            return;
          }
          HandleMaterialization(request_id, std::move(materialized), completion);
          gate->Leave();
        });
    if (!started) {
      std::uint64_t            late_task_id = 0;
      SaveCheckpointCompletion late_completion;
      if (TakePendingSave(request_id, &late_task_id, &late_completion)) {
        FinishSave(request_id, session_gen, late_task_id, false, "Materialization could not start",
                   late_completion);
      }
    }
    return;
  }

  const bool        ok  = outcome.accepted && outcome.durable;
  const std::string msg = outcome.error.empty()
                              ? (ok ? "Journal commit complete" : "Journal commit failed")
                              : outcome.error;
  FinishSave(request_id, session_gen, task_id, ok, msg, completion);
}

void EditorSaveCheckpointService::HandleMaterialization(std::uint64_t            request_id,
                                                        EditorMaterializeOutcome outcome,
                                                        SaveCheckpointCompletion completion) {
  std::uint64_t task_id     = 0;
  std::uint64_t session_gen = 0;
  bool          found       = false;
  {
    std::scoped_lock lock(mutex_);
    auto             it = std::find_if(
        pending_saves_.begin(), pending_saves_.end(),
        [request_id](const PendingSave& save) { return save.request_id == request_id; });
    if (it == pending_saves_.end()) {
      return;
    }
    found       = true;
    task_id     = it->task_id;
    session_gen = it->session_generation;
    pending_saves_.erase(it);
  }
  if (!found) {
    return;
  }
  const bool        ok = outcome.accepted && outcome.materialized;
  const std::string msg =
      outcome.error.empty()
          ? (outcome.materialized ? "Editor session materialized" : "Editor materialization failed")
          : outcome.error;
  FinishSave(request_id, session_gen, task_id, ok, msg, completion);
}

void EditorSaveCheckpointService::FinishSave(std::uint64_t request_id,
                                             std::uint64_t session_generation,
                                             std::uint64_t task_id, bool checkpoint_completed,
                                             std::string              message,
                                             SaveCheckpointCompletion completion) {
  if (deps_.tasks && task_id != 0) {
    deps_.tasks->EndTask(task_id, checkpoint_completed, message);
  }
  if (completion) {
    SaveCheckpointResult result;
    result.request_id           = request_id;
    result.session_generation   = session_generation;
    result.task_id              = task_id;
    result.checkpoint_completed = checkpoint_completed;
    result.error                = std::move(message);
    completion(result);
  }
}

auto EditorSaveCheckpointService::TakePendingSave(std::uint64_t request_id, std::uint64_t* task_id,
                                                  SaveCheckpointCompletion* completion) -> bool {
  std::scoped_lock lock(mutex_);
  auto             it =
      std::find_if(pending_saves_.begin(), pending_saves_.end(),
                   [request_id](const PendingSave& save) { return save.request_id == request_id; });
  if (it == pending_saves_.end()) {
    return false;
  }
  if (task_id) {
    *task_id = it->task_id;
  }
  if (completion) {
    *completion = std::move(it->completion);
  }
  pending_saves_.erase(it);
  return true;
}

}  // namespace alcedo
