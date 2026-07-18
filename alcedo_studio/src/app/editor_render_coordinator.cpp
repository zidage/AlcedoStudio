//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_render_coordinator.hpp"

#include <algorithm>
#include <utility>

namespace alcedo {

EditorRenderCoordinator::EditorRenderCoordinator(
    std::shared_ptr<IEditorPipelineSchedulerPort> scheduler)
    : scheduler_(std::move(scheduler)) {}

void EditorRenderCoordinator::SetResultObserver(ResultObserver observer) {
  observer_ = std::move(observer);
}

auto EditorRenderCoordinator::IsObsolete(const EditorRenderIntent& intent) const -> bool {
  return intent.session_generation != session_generation_ ||
         intent.render_generation != render_generation_ ||
         intent.view_generation != view_generation_;
}

void EditorRenderCoordinator::CancelObsoleteForActiveGenerations() {
  for (auto it = pending_.begin(); it != pending_.end();) {
    if (IsObsolete(it->request.intent)) {
      EditorRenderResult cancelled;
      cancelled.kind       = EditorRenderResultKind::Cancelled;
      cancelled.request_id = it->request.request_id;
      cancelled.intent     = it->request.intent;
      cancelled.message    = "Cancelled: obsolete generation";
      Emit(std::move(cancelled));
      terminal_request_ids_.insert(it->request.request_id);
      it = pending_.erase(it);
    } else {
      ++it;
    }
  }
  if (inflight_ && IsObsolete(inflight_->request.intent)) {
    if (scheduler_ && inflight_->scheduler_job_id != 0) {
      scheduler_->Cancel(inflight_->scheduler_job_id);
    }
    const std::uint64_t request_id = inflight_->request.request_id;
    EditorRenderResult  cancelled;
    cancelled.kind       = EditorRenderResultKind::Cancelled;
    cancelled.request_id = request_id;
    cancelled.intent     = inflight_->request.intent;
    cancelled.message    = "Cancelled in-flight: obsolete generation";
    Emit(std::move(cancelled));
    terminal_request_ids_.insert(request_id);
    inflight_.reset();
  }
  Pump();
}

void EditorRenderCoordinator::SetActiveGenerations(std::uint64_t session_generation,
                                                   std::uint64_t render_generation,
                                                   std::uint64_t view_generation) {
  session_generation_ = session_generation;
  render_generation_  = render_generation;
  view_generation_    = view_generation;
  CancelObsoleteForActiveGenerations();
}

auto EditorRenderCoordinator::AcceptOrReject(const EditorRenderIntent& intent,
                                             std::string*              message) const -> bool {
  if (intent.session_generation != session_generation_) {
    if (message) {
      *message = "Rejected: session generation mismatch";
    }
    return false;
  }
  if (intent.render_generation != render_generation_) {
    if (message) {
      *message = "Rejected: render generation mismatch";
    }
    return false;
  }
  if (intent.view_generation != view_generation_) {
    if (message) {
      *message = "Rejected: view generation mismatch";
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
  // Prefer QualityBase, then DetailPatch, then InteractivePrimary — matching the
  // reusable policy extracted from the legacy QWidget coordinator's StartNext().
  // Within the same role, higher EditorRenderPriority wins; stable order otherwise.
  std::size_t best = 0;
  auto role_rank = [](FrameRole role) -> int {
    switch (role) {
      case FrameRole::QualityBase:
        return 3;
      case FrameRole::DetailPatch:
        return 2;
      case FrameRole::InteractivePrimary:
        return 1;
    }
    return 0;
  };
  for (std::size_t i = 1; i < pending.size(); ++i) {
    const auto& a = pending[i].request.intent;
    const auto& b = pending[best].request.intent;
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
                                                    std::uint64_t       except_request_id) {
  if (key.empty()) {
    return;
  }
  for (auto it = pending_.begin(); it != pending_.end();) {
    if (it->request.request_id != except_request_id &&
        it->request.intent.replacement_key == key) {
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
  results_.push_back(result);
  if (observer_) {
    observer_(results_.back());
  }
}

auto EditorRenderCoordinator::HasResultKind(std::uint64_t request_id,
                                            EditorRenderResultKind kind) const -> bool {
  for (const auto& prior : results_) {
    if (prior.request_id == request_id && prior.kind == kind) {
      return true;
    }
  }
  return false;
}

auto EditorRenderCoordinator::Submit(const EditorRenderIntent& intent) -> EditorRenderResult {
  // Fill defaults on a local copy before accept so the stored request is immutable
  // after RequestAccepted (Phase 5A-Fix).
  EditorRenderIntent stamped = intent;
  FillRenderIntentDefaults(stamped);

  std::string message;
  if (!AcceptOrReject(stamped, &message)) {
    EditorRenderResult failed;
    failed.kind    = EditorRenderResultKind::Failed;
    failed.intent  = stamped;
    failed.message = std::move(message);
    Emit(failed);
    return failed;
  }

  EditorRenderRequest request;
  request.request_id = next_request_id_++;
  request.intent     = std::move(stamped);

  ReplacePendingWithKey(request.intent.replacement_key, request.request_id);

  PendingEntry entry;
  entry.request = request;
  pending_.push_back(std::move(entry));

  EditorRenderResult accepted;
  accepted.kind       = EditorRenderResultKind::RequestAccepted;
  accepted.request_id = request.request_id;
  accepted.intent     = request.intent;
  Emit(accepted);

  Pump();
  return accepted;
}

void EditorRenderCoordinator::CancelSession(std::uint64_t session_generation) {
  for (auto it = pending_.begin(); it != pending_.end();) {
    if (it->request.intent.session_generation == session_generation) {
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
  if (inflight_ && inflight_->request.intent.session_generation == session_generation) {
    if (scheduler_ && inflight_->scheduler_job_id != 0) {
      scheduler_->Cancel(inflight_->scheduler_job_id);
    }
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
  Pump();
}

auto EditorRenderCoordinator::CancelRequest(std::uint64_t request_id) -> bool {
  if (terminal_request_ids_.count(request_id) != 0) {
    return false;
  }
  for (auto it = pending_.begin(); it != pending_.end(); ++it) {
    if (it->request.request_id == request_id) {
      EditorRenderResult cancelled;
      cancelled.kind       = EditorRenderResultKind::Cancelled;
      cancelled.request_id = request_id;
      cancelled.intent     = it->request.intent;
      cancelled.message    = "Cancelled by request id";
      Emit(std::move(cancelled));
      terminal_request_ids_.insert(request_id);
      pending_.erase(it);
      Pump();
      return true;
    }
  }
  if (inflight_ && inflight_->request.request_id == request_id) {
    if (scheduler_ && inflight_->scheduler_job_id != 0) {
      scheduler_->Cancel(inflight_->scheduler_job_id);
    }
    EditorRenderResult cancelled;
    cancelled.kind       = EditorRenderResultKind::Cancelled;
    cancelled.request_id = request_id;
    cancelled.intent     = inflight_->request.intent;
    cancelled.message    = "Cancelled in-flight by request id";
    Emit(std::move(cancelled));
    terminal_request_ids_.insert(request_id);
    inflight_.reset();
    Pump();
    return true;
  }
  return false;
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
      cancelled.message    = "Cancelled: obsolete generation before schedule";
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

void EditorRenderCoordinator::Pump() { ScheduleNext(); }

void EditorRenderCoordinator::NotifySchedulerCompleted(std::uint64_t request_id, bool success,
                                                       std::string message) {
  if (terminal_request_ids_.count(request_id) != 0) {
    return;
  }
  if (!inflight_ || inflight_->request.request_id != request_id) {
    return;
  }
  EditorRenderResult completed;
  completed.kind       = success ? EditorRenderResultKind::RenderCompleted
                                 : EditorRenderResultKind::Failed;
  completed.request_id = request_id;
  completed.intent     = inflight_->request.intent;
  completed.message    = std::move(message);
  if (!success) {
    terminal_request_ids_.insert(request_id);
  }
  Emit(std::move(completed));
  inflight_.reset();
  Pump();
}

void EditorRenderCoordinator::NotifyFrameSubmitted(std::uint64_t request_id) {
  if (terminal_request_ids_.count(request_id) != 0) {
    return;
  }
  if (HasResultKind(request_id, EditorRenderResultKind::FrameSubmitted)) {
    return;
  }
  if (!HasResultKind(request_id, EditorRenderResultKind::RenderCompleted)) {
    return;
  }
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

void EditorRenderCoordinator::NotifyFramePresented(std::uint64_t request_id) {
  if (terminal_request_ids_.count(request_id) != 0) {
    return;
  }
  if (HasResultKind(request_id, EditorRenderResultKind::FramePresented)) {
    return;
  }
  // Presentation requires an ordered submit first (Phase 5A-Fix).
  if (!HasResultKind(request_id, EditorRenderResultKind::FrameSubmitted)) {
    return;
  }
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
}

}  // namespace alcedo
