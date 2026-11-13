//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_save_checkpoint_service.hpp"

#include <algorithm>
#include <functional>
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

EditorSaveCheckpointService::~EditorSaveCheckpointService() { CancelAndWait(); }

auto EditorSaveCheckpointService::TryAcquireSaveLock(sl_element_id_t element_id)
    -> EditorSaveCheckpointCoordinator::SaveCheckpointLock {
  if (!deps_.save_coordinator) {
    return EditorSaveCheckpointCoordinator::SaveCheckpointLock{};
  }
  return deps_.save_coordinator->TryAcquire(element_id);
}

auto EditorSaveCheckpointService::Start(SaveCheckpointRequest    request,
                                        SaveCheckpointCompletion completion) -> CheckpointTicket {
  // Hold the project-owned save lock through materialization and thumbnail
  // invalidation. Prefer a pre-acquired lock (taken before
  // capture on the GUI thread); otherwise TryAcquire here. Never block the GUI
  // thread.
  EditorSaveCheckpointCoordinator::SaveCheckpointLock save_lock = std::move(request.save_lock);
  if (!save_lock.owns_lock() && deps_.save_coordinator) {
    save_lock = deps_.save_coordinator->TryAcquire(request.element_id);
  }
  if (deps_.save_coordinator && !save_lock.owns_lock()) {
    if (completion) {
      SaveCheckpointResult result;
      result.operation_id         = request.operation_id;
      result.image_load_request_id = request.image_load_request_id;
      result.checkpoint_completed = false;
      result.error                = deps_.save_coordinator->is_shutdown()
                                        ? "Editor save checkpoint coordinator is shutting down"
                                        : "Another editor save checkpoint already owns the global save lock";
      DeliverCompletion(std::move(completion), std::move(result));
    }
    return CheckpointTicket{};
  }

  std::uint64_t task_id = 0;
  if (deps_.tasks) {
    task_id = deps_.tasks->BeginTask("editor_save", request.element_id);
    if (task_id == 0) {
      if (completion) {
        SaveCheckpointResult result;
        result.operation_id         = request.operation_id;
        result.image_load_request_id = request.image_load_request_id;
        result.checkpoint_completed = false;
        result.error                = "Failed to start editor save task";
        // Release lock before invoking completion so navigation can retry.
        save_lock.Release();
        DeliverCompletion(std::move(completion), std::move(result));
      }
      return CheckpointTicket{};
    }
  }

  const std::uint64_t request_id = [this] {
    std::scoped_lock lock(mutex_);
    return next_request_id_++;
  }();

  const auto capture = request.capture;
  {
    std::scoped_lock lock(mutex_);
    pending_saves_.push_back(PendingSave{
        request_id, request.operation_id, request.image_load_request_id, request.element_id, task_id,
        std::move(request.capture), request.last_journal_sequence, std::move(save_lock),
        completion});
  }

  const CheckpointTicket ticket{request_id, request.operation_id, request.image_load_request_id,
                                request.element_id, task_id};

  // Fail closed: a save without an immutable capture or without a checkpoint
  // store must not report a successful no-op materialization.
  std::string start_error;
  if (!deps_.checkpoint_store) {
    start_error = "Editor checkpoint store is unavailable";
  } else if (!capture) {
    start_error = "Save capture is required";
  } else {
    const auto gate    = callback_gate_;
    const bool started = deps_.checkpoint_store->MaterializeAsync(
        capture,
        [this, gate, request_id, completion](EditorMaterializeOutcome materialized) mutable {
          if (!gate || !gate->Enter()) {
            return;
          }
          HandleMaterialization(request_id, std::move(materialized), completion);
          gate->Leave();
        });
    if (started) {
      return ticket;
    }
    start_error = "Materialization could not start";
  }

  std::uint64_t                                       failed_task_id = 0;
  SaveCheckpointCompletion                            failed_completion;
  EditorSaveCheckpointCoordinator::SaveCheckpointLock failed_lock;
  if (TakePendingSave(request_id, &failed_task_id, &failed_completion, &failed_lock)) {
    FinishSave(request_id, request.operation_id, request.image_load_request_id, failed_task_id,
               false, std::move(start_error), failed_completion, std::move(failed_lock),
               std::nullopt);
  }
  // Without a command executor the failure completion is dropped, so the
  // caller learns about the failure only from the invalid ticket.
  return deps_.command_executor ? ticket : CheckpointTicket{};
}

void EditorSaveCheckpointService::CancelAndWait() {
  std::vector<PendingSave> abandoned;
  {
    std::scoped_lock lock(mutex_);
    abandoned.swap(pending_saves_);
  }
  // Publish exactly one terminal cancellation per abandoned request before
  // joining in-flight callbacks. Later storage completions find no
  // matching pending entry and cannot finish the task a second time.
  for (auto& save : abandoned) {
    if (deps_.tasks && save.task_id != 0) {
      deps_.tasks->EndTask(save.task_id, false, "Editor save checkpoint cancelled");
    }
    if (save.completion) {
      SaveCheckpointResult result;
      result.request_id           = save.request_id;
      result.operation_id         = save.operation_id;
      result.image_load_request_id = save.image_load_request_id;
      result.task_id              = save.task_id;
      result.checkpoint_completed = false;
      result.error                = "Editor save checkpoint cancelled";
      save.save_lock.Release();
      DeliverCompletion(std::move(save.completion), std::move(result));
    }
  }
  abandoned.clear();
  if (callback_gate_) {
    callback_gate_->StopAndWait();
  }
}

auto EditorSaveCheckpointService::active() const -> bool {
  std::scoped_lock lock(mutex_);
  return !pending_saves_.empty();
}

void EditorSaveCheckpointService::OnCheckpointFinished(const SaveCheckpointResult& result) {
  SaveCheckpointCompletion                            completion;
  EditorSaveCheckpointCoordinator::SaveCheckpointLock save_lock;
  std::uint64_t                                       task_id      = 0;
  std::uint64_t                                       operation_id = 0;
  {
    std::scoped_lock lock(mutex_);
    auto             it = std::find_if(pending_saves_.begin(), pending_saves_.end(),
                                       [&result](const PendingSave& save) {
                             return save.request_id == result.request_id &&
                                    save.image_load_request_id == result.image_load_request_id;
                           });
    if (it == pending_saves_.end()) {
      return;
    }
    task_id                  = it->task_id;
    operation_id             = it->operation_id;
    completion               = it->completion;
    save_lock                = std::move(it->save_lock);
    pending_saves_.erase(it);
  }
  if (deps_.tasks && task_id != 0) {
    deps_.tasks->EndTask(task_id, result.checkpoint_completed,
                         result.error.empty() ? "checkpoint finished" : result.error);
  }
  save_lock.Release();
  if (completion) {
    SaveCheckpointResult delivered = result;
    delivered.operation_id         = operation_id;
    DeliverCompletion(std::move(completion), std::move(delivered));
  }
}

void EditorSaveCheckpointService::HandleMaterialization(std::uint64_t            request_id,
                                                        EditorMaterializeOutcome outcome,
                                                        SaveCheckpointCompletion completion) {
  std::uint64_t                                       task_id      = 0;
  std::uint64_t                                       operation_id = 0;
  sl_element_id_t                                     element_id   = 0;
  ImageLoadRequestId                                  load_request_id{};
  bool                                                found        = false;
  std::optional<std::uint64_t>                        last_journal_sequence;
  EditorSaveCheckpointCoordinator::SaveCheckpointLock save_lock;
  {
    std::scoped_lock lock(mutex_);
    auto             it = std::find_if(
        pending_saves_.begin(), pending_saves_.end(),
        [request_id](const PendingSave& save) { return save.request_id == request_id; });
    if (it == pending_saves_.end()) {
      return;
    }
    found                    = true;
    task_id                  = it->task_id;
    operation_id             = it->operation_id;
    element_id               = it->element_id;
    load_request_id          = it->image_load_request_id;
    last_journal_sequence    = it->last_journal_sequence;
    save_lock                = std::move(it->save_lock);
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
  if (ok && deps_.thumbnails) {
    deps_.thumbnails->RefreshAfterMaterialization(element_id);
  }
  FinishSave(request_id, operation_id, load_request_id, task_id, ok, msg, completion,
             std::move(save_lock), ok ? last_journal_sequence : std::nullopt);
}

void EditorSaveCheckpointService::FinishSave(
    std::uint64_t request_id, std::uint64_t operation_id, ImageLoadRequestId image_load_request_id,
    std::uint64_t task_id, bool checkpoint_completed, std::string message,
    SaveCheckpointCompletion                              completion,
    EditorSaveCheckpointCoordinator::SaveCheckpointLock&& save_lock,
    std::optional<std::uint64_t> last_journal_sequence) {
  if (deps_.tasks && task_id != 0) {
    deps_.tasks->EndTask(task_id, checkpoint_completed, message);
  }
  // Navigation completion may open another image and recover its Mini-Git
  // journal. That recovery takes the same coordinator, so release after all
  // durable save work and before invoking the callback.
  save_lock.Release();
  if (completion) {
    SaveCheckpointResult result;
    result.request_id            = request_id;
    result.operation_id          = operation_id;
    result.image_load_request_id = image_load_request_id;
    result.task_id               = task_id;
    result.checkpoint_completed  = checkpoint_completed;
    result.error                 = std::move(message);
    result.last_journal_sequence = last_journal_sequence;
    DeliverCompletion(std::move(completion), std::move(result));
  }
}

void EditorSaveCheckpointService::DeliverCompletion(SaveCheckpointCompletion completion,
                                                    SaveCheckpointResult     result) {
  if (!completion) {
    return;
  }
  // CQ5: completions must never run on the service-start / worker stack. Unit
  // tests inject a manual executor; production injects the session queue adapter.
  if (!deps_.command_executor) {
    return;
  }
  deps_.command_executor->Post([completion = std::move(completion),
                                result = std::move(result)]() mutable { completion(result); });
}

auto EditorSaveCheckpointService::TakePendingSave(
    std::uint64_t request_id, std::uint64_t* task_id, SaveCheckpointCompletion* completion,
    EditorSaveCheckpointCoordinator::SaveCheckpointLock* save_lock) -> bool {
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
  if (save_lock) {
    *save_lock = std::move(it->save_lock);
  }
  pending_saves_.erase(it);
  return true;
}

}  // namespace alcedo
