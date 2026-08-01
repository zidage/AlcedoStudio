//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_render_coordinator.hpp"

#include <algorithm>
#include <utility>

namespace alcedo {

EditorRenderCoordinator::EditorRenderCoordinator(
    std::shared_ptr<IEditorPipelineSchedulerPort> scheduler)
    : scheduler_(std::move(scheduler)),
      cancellation_gate_(std::make_shared<CancellationCallbackGate>()) {
  cancellation_gate_->owner = this;
}

EditorRenderCoordinator::~EditorRenderCoordinator() {
  // A token may be cancelled from another thread. Clearing the gate waits for
  // any callback already using this coordinator and blocks all later callbacks.
  std::scoped_lock lock(cancellation_gate_->mutex);
  cancellation_gate_->owner = nullptr;
}

void EditorRenderCoordinator::SetResultObserver(ResultObserver observer) {
  std::scoped_lock lock(mutex_);
  observer_ = std::move(observer);
}

auto EditorRenderCoordinator::IsObsolete(const EditorRenderIntent& intent) const -> bool {
  return intent.image_load_request_id.value != active_image_load_request_id_;
}

auto EditorRenderCoordinator::CancelObsoleteForImageLoadMismatch() -> std::uint64_t {
  // Returns any in-flight scheduler job id that must be cancelled AFTER the
  // coordinator mutex is released. Production Cancel invokes the request token
  // callback, which re-enters CancelRequest and would deadlock if called while
  // mutex_ is held (zoom animation used to hit this on every DetailRefresh).
  std::uint64_t scheduler_job_to_cancel = 0;
  for (auto it = pending_.begin(); it != pending_.end();) {
    if (IsObsolete(it->request.intent)) {
      EditorRenderResult cancelled;
      cancelled.kind       = EditorRenderResultKind::Cancelled;
      cancelled.request_id = it->request.request_id;
      cancelled.intent     = it->request.intent;
      cancelled.message    = "Cancelled: obsolete image load request";
      Emit(std::move(cancelled));
      terminal_request_ids_.insert(it->request.request_id);
      it = pending_.erase(it);
    } else {
      ++it;
    }
  }
  if (inflight_ && IsObsolete(inflight_->request.intent)) {
    scheduler_job_to_cancel        = inflight_->scheduler_job_id;
    const std::uint64_t request_id = inflight_->request.request_id;
    EditorRenderResult  cancelled;
    cancelled.kind       = EditorRenderResultKind::Cancelled;
    cancelled.request_id = request_id;
    cancelled.intent     = inflight_->request.intent;
    cancelled.message    = "Cancelled in-flight: obsolete image load request";
    Emit(std::move(cancelled));
    terminal_request_ids_.insert(request_id);
    inflight_.reset();
  }
  ScheduleNext();
  return scheduler_job_to_cancel;
}

void EditorRenderCoordinator::SetActiveImageLoadRequest(std::uint64_t image_load_request_id) {
  std::uint64_t scheduler_job_to_cancel = 0;
  {
    std::scoped_lock lock(mutex_);
    if (active_image_load_request_id_ == image_load_request_id) {
      return;
    }
    active_image_load_request_id_ = image_load_request_id;
    scheduler_job_to_cancel       = CancelObsoleteForImageLoadMismatch();
  }
  // Token cancel callbacks re-enter CancelRequest; never hold mutex_ here.
  if (scheduler_ && scheduler_job_to_cancel != 0) {
    scheduler_->Cancel(scheduler_job_to_cancel);
  }
  DeliverPendingResults();
}

auto EditorRenderCoordinator::AcceptOrReject(const EditorRenderIntent& intent,
                                             std::string*              message) const -> bool {
  if (intent.image_load_request_id.value != active_image_load_request_id_) {
    if (message) {
      *message = "Rejected: image load request mismatch";
    }
    return false;
  }
  if (intent.cancellation && intent.cancellation->IsCancelled()) {
    if (message) {
      *message = "Rejected: cancellation token already set";
    }
    return false;
  }
  if (!scheduler_) {
    if (message) {
      *message = "Rejected: no scheduler port";
    }
    return false;
  }
  return true;
}

auto EditorRenderCoordinator::PriorityRank(EditorRenderPriority priority) -> int {
  switch (priority) {
    case EditorRenderPriority::High:
      return 3;
    case EditorRenderPriority::Normal:
      return 2;
    case EditorRenderPriority::Low:
      return 1;
  }
  return 0;
}

auto EditorRenderCoordinator::SelectNextIndex(const std::deque<PendingEntry>& pending)
    -> std::size_t {
  // Phase 5D priority order for visible work: missing first frame / current
  // interactive response (InteractivePrimary) first, then settled QualityBase,
  // then the current detail patch, then background/non-visible work. Within the
  // same role, higher EditorRenderPriority wins; stable order otherwise. This
  // guarantees interactive work is never blocked behind an outdated quality or
  // detail request.
  std::size_t best      = 0;
  auto        role_rank = [](FrameRole role) -> int {
    switch (role) {
      case FrameRole::InteractivePrimary:
        return 3;
      case FrameRole::QualityBase:
        return 2;
      case FrameRole::DetailPatch:
        return 1;
    }
    return 0;
  };
  for (std::size_t i = 1; i < pending.size(); ++i) {
    const auto& a  = pending[i].request.intent;
    const auto& b  = pending[best].request.intent;
    const int   ra = role_rank(a.frame_role);
    const int   rb = role_rank(b.frame_role);
    if (ra > rb) {
      best = i;
      continue;
    }
    if (ra < rb) {
      continue;
    }
    if (PriorityRank(a.priority) > PriorityRank(b.priority)) {
      best = i;
    }
  }
  return best;
}

void EditorRenderCoordinator::ReplacePendingWithKey(const std::string& key,
                                                    std::uint64_t      except_request_id) {
  if (key.empty()) {
    return;
  }
  for (auto it = pending_.begin(); it != pending_.end();) {
    if (it->request.request_id != except_request_id && it->request.intent.replacement_key == key) {
      EditorRenderResult replaced;
      replaced.kind       = EditorRenderResultKind::Replaced;
      replaced.request_id = it->request.request_id;
      replaced.intent     = it->request.intent;
      replaced.message    = "Replaced by newer intent with the same replacement key";
      Emit(std::move(replaced));
      terminal_request_ids_.insert(it->request.request_id);
      it = pending_.erase(it);
    } else {
      ++it;
    }
  }
}

void EditorRenderCoordinator::Emit(EditorRenderResult result) {
  switch (result.kind) {
    case EditorRenderResultKind::RequestAccepted:
      ++accepted_count_;
      break;
    case EditorRenderResultKind::Replaced:
      ++replaced_count_;
      break;
    case EditorRenderResultKind::Cancelled:
      ++cancelled_count_;
      break;
    case EditorRenderResultKind::Failed:
      ++failed_count_;
      last_error_ = result.message.empty() ? "Render failed" : result.message;
      // Submit-time rejections also use Failed with a "Rejected:" message.
      if (result.message.rfind("Rejected:", 0) == 0) {
        last_rejection_reason_       = last_error_;
        last_rejected_render_reason_ = result.intent.reason;
      }
      break;
    case EditorRenderResultKind::FrameSubmitted:
      last_submitted_frame_role_    = result.intent.frame_role;
      last_submitted_render_reason_ = result.intent.reason;
      break;
    case EditorRenderResultKind::FramePresented:
      ++presented_count_;
      break;
    default:
      break;
  }
  results_.push_back(result);
  if (observer_) {
    pending_delivery_.push_back(results_.back());
  }
}

void EditorRenderCoordinator::DeliverPendingResults() {
  // Claim one delivery owner without holding a lock across observer callbacks.
  // An observer may wait for the session mutex while a slider submit holds that
  // mutex and queues a replacement render. Waiting for another delivery lock in
  // that submit path would invert the locks and deadlock continuous input.
  {
    std::scoped_lock lock(mutex_);
    if (delivery_in_progress_) {
      return;
    }
    delivery_in_progress_ = true;
  }

  for (;;) {
    ResultObserver                  observer;
    std::vector<EditorRenderResult> batch;
    {
      std::scoped_lock lock(mutex_);
      if (pending_delivery_.empty()) {
        delivery_in_progress_ = false;
        return;
      }
      observer = observer_;
      batch.swap(pending_delivery_);
    }
    if (!observer) {
      continue;
    }
    for (const auto& result : batch) {
      try {
        observer(result);
      } catch (...) {
        std::scoped_lock lock(mutex_);
        delivery_in_progress_ = false;
        throw;
      }
    }
  }
}

auto EditorRenderCoordinator::HasResultKind(std::uint64_t          request_id,
                                            EditorRenderResultKind kind) const -> bool {
  for (const auto& prior : results_) {
    if (prior.request_id == request_id && prior.kind == kind) {
      return true;
    }
  }
  return false;
}

auto EditorRenderCoordinator::Submit(const EditorRenderIntent& intent) -> EditorRenderResult {
  EditorRenderResult                             result;
  std::shared_ptr<EditorRenderCancellationToken> cancellation;
  std::uint64_t                                  request_id = 0;
  std::weak_ptr<CancellationCallbackGate>        weak_gate;
  {
    std::scoped_lock   lock(mutex_);
    // Fill defaults on a local copy before accept so the stored request is immutable
    // after RequestAccepted (Phase 5A-Fix).
    EditorRenderIntent stamped = intent;
    FillRenderIntentDefaults(stamped);

    std::string message;
    if (!AcceptOrReject(stamped, &message)) {
      result.kind    = EditorRenderResultKind::Failed;
      result.intent  = std::move(stamped);
      result.message = std::move(message);
      Emit(result);
    } else if (ReasonReusesCurrentFrame(stamped.reason)) {
      // Phase 5D coordinator decision: a pure view-transform change (zoom/pan/
      // resize) reuses the current full frame. The renderer re-samples it through
      // the item-to-renderer synchronize() path, so no pipeline task is needed.
      // Input handlers only reported the new view; the coordinator chose reuse.
      result.kind    = EditorRenderResultKind::Reused;
      result.intent  = std::move(stamped);
      result.message = "Reused current frame; viewport re-samples the view";
      Emit(result);
    } else {
      EditorRenderRequest request;
      request.request_id = next_request_id_++;
      request.intent     = std::move(stamped);

      ReplacePendingWithKey(request.intent.replacement_key, request.request_id);

      PendingEntry entry;
      entry.request = request;
      pending_.push_back(std::move(entry));

      result.kind       = EditorRenderResultKind::RequestAccepted;
      result.request_id = request.request_id;
      result.intent     = request.intent;
      Emit(result);

      cancellation = request.intent.cancellation;
      request_id   = request.request_id;
      weak_gate    = cancellation_gate_;
      ScheduleNext();
    }
  }

  DeliverPendingResults();
  if (cancellation) {
    // Install the cross-thread cancellation bridge after releasing the queue lock.
    // SetCancelCallback handles cancellation that raced with request acceptance.
    cancellation->SetCancelCallback([weak_gate, request_id] {
      if (const auto gate = weak_gate.lock()) {
        std::scoped_lock lock(gate->mutex);
        if (gate->owner) {
          gate->owner->CancelRequest(request_id);
        }
      }
    });
  }
  return result;
}

void EditorRenderCoordinator::CancelSession(std::uint64_t image_load_request_id) {
  std::uint64_t scheduler_job_to_cancel = 0;
  {
    std::scoped_lock lock(mutex_);
    for (auto it = pending_.begin(); it != pending_.end();) {
      if (it->request.intent.image_load_request_id.value == image_load_request_id) {
        EditorRenderResult cancelled;
        cancelled.kind       = EditorRenderResultKind::Cancelled;
        cancelled.request_id = it->request.request_id;
        cancelled.intent     = it->request.intent;
        cancelled.message    = "Cancelled with session";
        Emit(std::move(cancelled));
        terminal_request_ids_.insert(it->request.request_id);
        it = pending_.erase(it);
      } else {
        ++it;
      }
    }
    if (inflight_ && inflight_->request.intent.image_load_request_id.value == image_load_request_id) {
      scheduler_job_to_cancel        = inflight_->scheduler_job_id;
      const std::uint64_t request_id = inflight_->request.request_id;
      EditorRenderResult  cancelled;
      cancelled.kind       = EditorRenderResultKind::Cancelled;
      cancelled.request_id = request_id;
      cancelled.intent     = inflight_->request.intent;
      cancelled.message    = "Cancelled in-flight with session";
      Emit(std::move(cancelled));
      terminal_request_ids_.insert(request_id);
      inflight_.reset();
    }
    // Start any pending work that is still valid after the cancel.
    ScheduleNext();
  }
  // Production Cancel propagates the request token. Its callback re-enters
  // CancelRequest, so it must run outside the coordinator mutex.
  if (scheduler_ && scheduler_job_to_cancel != 0) {
    scheduler_->Cancel(scheduler_job_to_cancel);
  }
  DeliverPendingResults();
}

void EditorRenderCoordinator::CancelSessionAndWait(std::uint64_t image_load_request_id) {
  CancelSession(image_load_request_id);
  if (scheduler_) {
    scheduler_->WaitForSessionIdle(image_load_request_id);
  }
}

auto EditorRenderCoordinator::CancelRequest(std::uint64_t request_id) -> bool {
  bool          did_cancel              = false;
  std::uint64_t scheduler_job_to_cancel = 0;
  {
    std::scoped_lock lock(mutex_);
    if (terminal_request_ids_.count(request_id) == 0) {
      for (auto it = pending_.begin(); it != pending_.end(); ++it) {
        if (it->request.request_id != request_id) {
          continue;
        }
        EditorRenderResult cancelled;
        cancelled.kind       = EditorRenderResultKind::Cancelled;
        cancelled.request_id = request_id;
        cancelled.intent     = it->request.intent;
        cancelled.message    = "Cancelled by request id";
        Emit(std::move(cancelled));
        terminal_request_ids_.insert(request_id);
        pending_.erase(it);
        ScheduleNext();
        did_cancel = true;
        break;
      }
    }
    if (!did_cancel && terminal_request_ids_.count(request_id) == 0 && inflight_ &&
        inflight_->request.request_id == request_id) {
      // Defer scheduler Cancel until after mutex_ release. Production Cancel
      // fires the token callback which re-enters CancelRequest.
      scheduler_job_to_cancel = inflight_->scheduler_job_id;
      EditorRenderResult cancelled;
      cancelled.kind       = EditorRenderResultKind::Cancelled;
      cancelled.request_id = request_id;
      cancelled.intent     = inflight_->request.intent;
      cancelled.message    = "Cancelled in-flight by request id";
      Emit(std::move(cancelled));
      terminal_request_ids_.insert(request_id);
      inflight_.reset();
      ScheduleNext();
      did_cancel = true;
    }
  }
  if (scheduler_ && scheduler_job_to_cancel != 0) {
    scheduler_->Cancel(scheduler_job_to_cancel);
  }
  DeliverPendingResults();
  return did_cancel;
}

void EditorRenderCoordinator::ScheduleNext() {
  if (inflight_ || pending_.empty() || !scheduler_) {
    return;
  }

  // Drop cancelled pending entries before selecting.
  for (auto it = pending_.begin(); it != pending_.end();) {
    if (it->request.intent.cancellation && it->request.intent.cancellation->IsCancelled()) {
      EditorRenderResult cancelled;
      cancelled.kind       = EditorRenderResultKind::Cancelled;
      cancelled.request_id = it->request.request_id;
      cancelled.intent     = it->request.intent;
      cancelled.message    = "Cancelled by token before schedule";
      Emit(std::move(cancelled));
      terminal_request_ids_.insert(it->request.request_id);
      it = pending_.erase(it);
    } else if (IsObsolete(it->request.intent)) {
      EditorRenderResult cancelled;
      cancelled.kind       = EditorRenderResultKind::Cancelled;
      cancelled.request_id = it->request.request_id;
      cancelled.intent     = it->request.intent;
      cancelled.message    = "Cancelled: obsolete image load request before schedule";
      Emit(std::move(cancelled));
      terminal_request_ids_.insert(it->request.request_id);
      it = pending_.erase(it);
    } else {
      ++it;
    }
  }
  if (pending_.empty()) {
    return;
  }

  const std::size_t index = SelectNextIndex(pending_);
  PendingEntry      entry = pending_[index];
  pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(index));

  const std::uint64_t job_id = scheduler_->Schedule(entry.request);
  if (job_id == 0) {
    EditorRenderResult failed;
    failed.kind       = EditorRenderResultKind::Failed;
    failed.request_id = entry.request.request_id;
    failed.intent     = entry.request.intent;
    failed.message    = "Scheduler rejected the request";
    Emit(std::move(failed));
    terminal_request_ids_.insert(entry.request.request_id);
    ScheduleNext();
    return;
  }

  entry.scheduler_job_id     = job_id;
  last_scheduled_request_id_ = entry.request.request_id;
  inflight_                  = std::move(entry);

  EditorRenderResult started;
  started.kind       = EditorRenderResultKind::RenderStarted;
  started.request_id = inflight_->request.request_id;
  started.intent     = inflight_->request.intent;
  Emit(std::move(started));
}

void EditorRenderCoordinator::Pump() {
  {
    std::scoped_lock lock(mutex_);
    ScheduleNext();
  }
  DeliverPendingResults();
}

void EditorRenderCoordinator::NotifySchedulerCompleted(std::uint64_t request_id, bool success,
                                                       std::string message) {
  {
    std::scoped_lock lock(mutex_);
    if (terminal_request_ids_.count(request_id) == 0 && inflight_ &&
        inflight_->request.request_id == request_id) {
      EditorRenderResult completed;
      completed.kind =
          success ? EditorRenderResultKind::RenderCompleted : EditorRenderResultKind::Failed;
      completed.request_id = request_id;
      completed.intent     = inflight_->request.intent;
      completed.message    = std::move(message);
      if (!success) {
        terminal_request_ids_.insert(request_id);
      }
      Emit(std::move(completed));
      inflight_.reset();
      ScheduleNext();
    }
  }
  DeliverPendingResults();
}

void EditorRenderCoordinator::NotifyFrameSubmitted(std::uint64_t request_id) {
  {
    std::scoped_lock lock(mutex_);
    if (terminal_request_ids_.count(request_id) == 0 &&
        !HasResultKind(request_id, EditorRenderResultKind::FrameSubmitted) &&
        HasResultKind(request_id, EditorRenderResultKind::RenderCompleted)) {
      EditorRenderIntent intent{};
      for (const auto& prior : results_) {
        if (prior.request_id == request_id &&
            prior.kind == EditorRenderResultKind::RenderCompleted) {
          intent = prior.intent;
          break;
        }
      }
      EditorRenderResult submitted;
      submitted.kind       = EditorRenderResultKind::FrameSubmitted;
      submitted.request_id = request_id;
      submitted.intent     = intent;
      Emit(std::move(submitted));
    }
  }
  DeliverPendingResults();
}

void EditorRenderCoordinator::NotifyFramePresented(std::uint64_t request_id) {
  {
    std::scoped_lock lock(mutex_);
    if (terminal_request_ids_.count(request_id) == 0 &&
        !HasResultKind(request_id, EditorRenderResultKind::FramePresented) &&
        HasResultKind(request_id, EditorRenderResultKind::FrameSubmitted)) {
      EditorRenderIntent intent{};
      for (const auto& prior : results_) {
        if (prior.request_id == request_id &&
            prior.kind == EditorRenderResultKind::FrameSubmitted) {
          intent = prior.intent;
          break;
        }
      }
      EditorRenderResult presented;
      presented.kind       = EditorRenderResultKind::FramePresented;
      presented.request_id = request_id;
      presented.intent     = intent;
      Emit(std::move(presented));
      terminal_request_ids_.insert(request_id);
    }
  }
  DeliverPendingResults();
}

}  // namespace alcedo
