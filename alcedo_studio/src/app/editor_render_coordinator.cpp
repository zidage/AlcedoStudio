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

void EditorRenderCoordinator::SetActiveGenerations(std::uint64_t session_generation,
                                                   std::uint64_t render_generation,
                                                   std::uint64_t view_generation) {
  session_generation_ = session_generation;
  render_generation_  = render_generation;
  view_generation_    = view_generation;
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

auto EditorRenderCoordinator::Submit(const EditorRenderIntent& intent) -> EditorRenderResult {
  std::string message;
  if (!AcceptOrReject(intent, &message)) {
    EditorRenderResult failed;
    failed.kind    = EditorRenderResultKind::Failed;
    failed.intent  = intent;
    failed.message = std::move(message);
    Emit(failed);
    return failed;
  }

  EditorRenderRequest request;
  request.request_id = next_request_id_++;
  request.intent     = intent;
  if (request.intent.replacement_key.empty()) {
    request.intent.replacement_key = DefaultReplacementKey(request.intent.quality);
  }
  if (request.intent.frame_role == FrameRole::InteractivePrimary &&
      request.intent.quality != EditorRenderQuality::Interactive) {
    request.intent.frame_role = FrameRoleForQuality(request.intent.quality);
  }

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
      it = pending_.erase(it);
    } else {
      ++it;
    }
  }
  if (inflight_ && inflight_->request.intent.session_generation == session_generation) {
    if (scheduler_ && inflight_->scheduler_job_id != 0) {
      scheduler_->Cancel(inflight_->scheduler_job_id);
    }
    EditorRenderResult cancelled;
    cancelled.kind       = EditorRenderResultKind::Cancelled;
    cancelled.request_id = inflight_->request.request_id;
    cancelled.intent     = inflight_->request.intent;
    cancelled.message    = "Cancelled in-flight with session";
    Emit(std::move(cancelled));
    inflight_.reset();
  }
}

auto EditorRenderCoordinator::CancelRequest(std::uint64_t request_id) -> bool {
  for (auto it = pending_.begin(); it != pending_.end(); ++it) {
    if (it->request.request_id == request_id) {
      EditorRenderResult cancelled;
      cancelled.kind       = EditorRenderResultKind::Cancelled;
      cancelled.request_id = request_id;
      cancelled.intent     = it->request.intent;
      cancelled.message    = "Cancelled by request id";
      Emit(std::move(cancelled));
      pending_.erase(it);
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
    inflight_.reset();
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
    ScheduleNext();
    return;
  }

  entry.scheduler_job_id      = job_id;
  last_scheduled_request_id_  = entry.request.request_id;
  inflight_                   = std::move(entry);

  EditorRenderResult started;
  started.kind       = EditorRenderResultKind::RenderStarted;
  started.request_id = inflight_->request.request_id;
  started.intent     = inflight_->request.intent;
  Emit(std::move(started));
}

void EditorRenderCoordinator::Pump() { ScheduleNext(); }

void EditorRenderCoordinator::NotifySchedulerCompleted(std::uint64_t request_id, bool success,
                                                       std::string message) {
  if (!inflight_ || inflight_->request.request_id != request_id) {
    return;
  }
  EditorRenderResult completed;
  completed.kind       = success ? EditorRenderResultKind::RenderCompleted
                                 : EditorRenderResultKind::Failed;
  completed.request_id = request_id;
  completed.intent     = inflight_->request.intent;
  completed.message    = std::move(message);
  Emit(std::move(completed));
  inflight_.reset();
  Pump();
}

void EditorRenderCoordinator::NotifyFrameSubmitted(std::uint64_t request_id) {
  for (const auto& prior : results_) {
    if (prior.request_id == request_id &&
        prior.kind == EditorRenderResultKind::RenderCompleted) {
      EditorRenderResult submitted;
      submitted.kind       = EditorRenderResultKind::FrameSubmitted;
      submitted.request_id = request_id;
      submitted.intent     = prior.intent;
      Emit(std::move(submitted));
      return;
    }
  }
}

void EditorRenderCoordinator::NotifyFramePresented(std::uint64_t request_id) {
  for (const auto& prior : results_) {
    if (prior.request_id == request_id &&
        (prior.kind == EditorRenderResultKind::FrameSubmitted ||
         prior.kind == EditorRenderResultKind::RenderCompleted)) {
      EditorRenderResult presented;
      presented.kind       = EditorRenderResultKind::FramePresented;
      presented.request_id = request_id;
      presented.intent     = prior.intent;
      Emit(std::move(presented));
      return;
    }
  }
}

}  // namespace alcedo
