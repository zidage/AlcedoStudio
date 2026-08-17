//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_render_coordinator.hpp"

#include "utils/diagnostics/render_e2e_timing.hpp"

#include <algorithm>
#include <utility>

namespace alcedo {
namespace {

auto ReasonLabel(const EditorRenderReason reason) -> const char* {
  switch (reason) {
    case EditorRenderReason::InitialFrame:
      return "InitialFrame";
    case EditorRenderReason::InteractiveAdjustment:
      return "InteractiveAdjustment";
    case EditorRenderReason::SettledAdjustment:
      return "SettledAdjustment";
    case EditorRenderReason::ZoomPan:
      return "ZoomPan";
    case EditorRenderReason::Resize:
      return "Resize";
    case EditorRenderReason::DetailRefresh:
      return "DetailRefresh";
    case EditorRenderReason::UndoRedo:
      return "UndoRedo";
    case EditorRenderReason::ImageSwitch:
      return "ImageSwitch";
    case EditorRenderReason::Retry:
      return "Retry";
    case EditorRenderReason::CropRotate:
      return "CropRotate";
    case EditorRenderReason::ScopeRefresh:
      return "ScopeRefresh";
  }
  return "?";
}

auto QualityLabel(const EditorRenderQuality quality) -> const char* {
  switch (quality) {
    case EditorRenderQuality::Interactive:
      return "Interactive";
    case EditorRenderQuality::Quality:
      return "Quality";
    case EditorRenderQuality::Detail:
      return "Detail";
  }
  return "?";
}

auto RoleLabel(const FrameRole role) -> const char* {
  switch (role) {
    case FrameRole::InteractivePrimary:
      return "InteractivePrimary";
    case FrameRole::QualityBase:
      return "QualityBase";
    case FrameRole::DetailPatch:
      return "DetailPatch";
  }
  return "?";
}

void NoteE2eSubmit(const EditorRenderRequest& request) {
  diag::NoteRenderE2eSubmit(request.request_id, ReasonLabel(request.intent.reason),
                            QualityLabel(request.intent.quality),
                            RoleLabel(request.intent.frame_role));
}

}  // namespace

EditorRenderCoordinator::EditorRenderCoordinator(
    std::shared_ptr<IEditorPipelineSchedulerPort> scheduler)
    : scheduler_(std::move(scheduler)) {}

void EditorRenderCoordinator::SetResultObserver(ResultObserver observer) {
  std::scoped_lock lock(mutex_);
  observer_ = std::move(observer);
}

auto EditorRenderCoordinator::SlotIndexForQuality(EditorRenderQuality quality) -> std::size_t {
  switch (quality) {
    case EditorRenderQuality::Interactive:
      return static_cast<std::size_t>(QualitySlot::Interactive);
    case EditorRenderQuality::Quality:
      return static_cast<std::size_t>(QualitySlot::Quality);
    case EditorRenderQuality::Detail:
      return static_cast<std::size_t>(QualitySlot::Detail);
  }
  return static_cast<std::size_t>(QualitySlot::Interactive);
}

auto EditorRenderCoordinator::CountOccupiedSlots() const -> std::size_t {
  std::size_t n = 0;
  for (const auto& slot : slots_) {
    if (slot.has_value()) {
      ++n;
    }
  }
  return n;
}

auto EditorRenderCoordinator::SelectNextSlotIndex() const -> std::optional<std::size_t> {
  // Visible work order: interactive, settled quality, then detail patch.
  for (std::size_t i = 0; i < kQualitySlotCount; ++i) {
    if (slots_[i].has_value()) {
      return i;
    }
  }
  return std::nullopt;
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
  for (auto& slot : slots_) {
    if (!slot.has_value() || !IsObsolete(slot->request.intent)) {
      continue;
    }
    EditorRenderResult cancelled;
    cancelled.kind       = EditorRenderResultKind::Cancelled;
    cancelled.request_id = slot->request.request_id;
    cancelled.intent     = slot->request.intent;
    cancelled.message    = "Cancelled: obsolete image load request";
    Emit(std::move(cancelled));
    terminal_request_ids_.insert(slot->request.request_id);
    slot.reset();
  }
  if (inflight_ && IsObsolete(inflight_->request.intent)) {
    scheduler_job_to_cancel        = inflight_->scheduler_job_id;
    const std::uint64_t request_id = inflight_->request.request_id;
    if (inflight_->request.intent.cancellation) {
      inflight_->request.intent.cancellation->Cancel();
    }
    EditorRenderResult cancelled;
    cancelled.kind       = EditorRenderResultKind::Cancelled;
    cancelled.request_id = request_id;
    cancelled.intent     = inflight_->request.intent;
    cancelled.message    = "Cancelled in-flight: obsolete image load request";
    Emit(std::move(cancelled));
    terminal_request_ids_.insert(request_id);
  }
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
  if (scheduler_ && scheduler_job_to_cancel != 0) {
    scheduler_->Cancel(scheduler_job_to_cancel);
  }
  DeliverPendingResults();
}

void EditorRenderCoordinator::BindSessionRenderContext(std::uint64_t epoch,
                                                       sl_element_id_t element_id,
                                                       image_id_t image_id,
                                                       PresentationSinkId presentation_sink_id) {
  if (scheduler_) {
    scheduler_->BindSessionContext(epoch, element_id, image_id, presentation_sink_id);
  }
}

void EditorRenderCoordinator::ClearSessionRenderContext() {
  if (scheduler_) {
    scheduler_->ClearSessionContext();
  }
}

void EditorRenderCoordinator::SetPipelineSchedulerPort(
    std::shared_ptr<IEditorPipelineSchedulerPort> scheduler) {
  std::scoped_lock lock(mutex_);
  scheduler_ = std::move(scheduler);
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

void EditorRenderCoordinator::PlaceInSlot(std::size_t slot, PendingEntry entry) {
  if (slot >= kQualitySlotCount) {
    slot = static_cast<std::size_t>(QualitySlot::Interactive);
  }
  if (slots_[slot].has_value()) {
    EditorRenderResult replaced;
    replaced.kind       = EditorRenderResultKind::Replaced;
    replaced.request_id = slots_[slot]->request.request_id;
    replaced.intent     = slots_[slot]->request.intent;
    replaced.message    = "Replaced by newer intent in the same quality slot";
    Emit(std::move(replaced));
    terminal_request_ids_.insert(slots_[slot]->request.request_id);
  }
  slots_[slot] = std::move(entry);
}

void EditorRenderCoordinator::Emit(EditorRenderResult result) {
  switch (result.kind) {
    case EditorRenderResultKind::RequestAccepted:
      ++accepted_count_;
      break;
    case EditorRenderResultKind::Replaced:
      ++replaced_count_;
      if (result.request_id != 0) {
        diag::NoteRenderE2eTerminal(result.request_id, "replaced");
      }
      break;
    case EditorRenderResultKind::Cancelled:
      ++cancelled_count_;
      if (result.request_id != 0) {
        diag::NoteRenderE2eTerminal(result.request_id, "cancelled");
      }
      break;
    case EditorRenderResultKind::Failed:
      ++failed_count_;
      last_error_ = result.message.empty() ? "Render failed" : result.message;
      // Submit-time rejections also use Failed with a "Rejected:" message.
      if (result.message.rfind("Rejected:", 0) == 0) {
        last_rejection_reason_       = last_error_;
        last_rejected_render_reason_ = result.intent.reason;
      }
      if (result.request_id != 0) {
        diag::NoteRenderE2eTerminal(result.request_id, "failed");
      }
      break;
    case EditorRenderResultKind::FrameReady:
      last_ready_frame_role_    = result.intent.frame_role;
      last_ready_render_reason_ = result.intent.reason;
      ++ready_count_;
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

auto EditorRenderCoordinator::Submit(const EditorRenderIntent& intent) -> EditorRenderResult {
  EditorRenderResult result;
  {
    std::scoped_lock lock(mutex_);
    // Fill defaults before storing the immutable accepted request.
    EditorRenderIntent stamped = intent;
    FillRenderIntentDefaults(stamped);

    std::string message;
    if (!AcceptOrReject(stamped, &message)) {
      result.kind    = EditorRenderResultKind::Failed;
      result.intent  = std::move(stamped);
      result.message = std::move(message);
      Emit(result);
    } else if (ReasonReusesCurrentFrame(stamped.reason)) {
      // A pure view-transform change reuses the current full frame; the renderer
      // re-samples it without a pipeline task.
      result.kind    = EditorRenderResultKind::Reused;
      result.intent  = std::move(stamped);
      result.message = "Reused current frame; viewport re-samples the view";
      Emit(result);
    } else {
      EditorRenderRequest request;
      request.request_id = next_request_id_++;
      request.intent     = std::move(stamped);
      NoteE2eSubmit(request);

      PendingEntry entry;
      entry.request = request;
      PlaceInSlot(SlotIndexForQuality(request.intent.quality), std::move(entry));

      result.kind       = EditorRenderResultKind::RequestAccepted;
      result.request_id = request.request_id;
      result.intent     = request.intent;
      Emit(result);

      ScheduleNext();
    }
  }

  DeliverPendingResults();
  return result;
}

void EditorRenderCoordinator::CancelSession(std::uint64_t image_load_request_id) {
  CancelSession(image_load_request_id, {});
}

void EditorRenderCoordinator::CancelSession(std::uint64_t image_load_request_id,
                                            SessionIdleCallback on_idle) {
  std::uint64_t scheduler_job_to_cancel = 0;
  std::vector<SessionIdleCallback> ready_callbacks;
  {
    std::scoped_lock lock(mutex_);
    for (auto& slot : slots_) {
      if (!slot.has_value() ||
          slot->request.intent.image_load_request_id.value != image_load_request_id) {
        continue;
      }
      EditorRenderResult cancelled;
      cancelled.kind       = EditorRenderResultKind::Cancelled;
      cancelled.request_id = slot->request.request_id;
      cancelled.intent     = slot->request.intent;
      cancelled.message    = "Cancelled with session";
      Emit(std::move(cancelled));
      terminal_request_ids_.insert(slot->request.request_id);
      slot.reset();
    }
    if (inflight_ && inflight_->request.intent.image_load_request_id.value == image_load_request_id) {
      scheduler_job_to_cancel        = inflight_->scheduler_job_id;
      const std::uint64_t request_id = inflight_->request.request_id;
      if (inflight_->request.intent.cancellation) {
        inflight_->request.intent.cancellation->Cancel();
      }
      EditorRenderResult cancelled;
      cancelled.kind       = EditorRenderResultKind::Cancelled;
      cancelled.request_id = request_id;
      cancelled.intent     = inflight_->request.intent;
      cancelled.message    = "Cancelled in-flight with session";
      Emit(std::move(cancelled));
      terminal_request_ids_.insert(request_id);
    }
    // Pending-only cancellation may leave the coordinator idle. A cancelled
    // running request remains in-flight until the blocking pipeline call exits.
    ScheduleNext();
    if (on_idle) {
      if (HasSessionWork(image_load_request_id)) {
        idle_callbacks_.push_back(
            PendingIdleCallback{image_load_request_id, std::move(on_idle)});
      } else {
        ready_callbacks.push_back(std::move(on_idle));
      }
    }
  }
  if (scheduler_ && scheduler_job_to_cancel != 0) {
    scheduler_->Cancel(scheduler_job_to_cancel);
  }
  DeliverPendingResults();
  for (auto& callback : ready_callbacks) {
    callback(image_load_request_id);
  }
}

void EditorRenderCoordinator::CancelSessionAndWait(std::uint64_t image_load_request_id) {
  CancelSession(image_load_request_id);
  if (scheduler_) {
    scheduler_->WaitForSessionIdle(image_load_request_id);
  }
}

void EditorRenderCoordinator::WaitForSessionIdle(std::uint64_t image_load_request_id) {
  // History head moves queue behind the *current* frame only:
  // - Drop not-yet-started pending (stale after rebuild; no point finishing them).
  // - Do not cancel in-flight work — let it present, then take render_lock.
  {
    std::scoped_lock lock(mutex_);
    for (auto& slot : slots_) {
      if (!slot.has_value() ||
          slot->request.intent.image_load_request_id.value != image_load_request_id) {
        continue;
      }
      EditorRenderResult cancelled;
      cancelled.kind       = EditorRenderResultKind::Cancelled;
      cancelled.request_id = slot->request.request_id;
      cancelled.intent     = slot->request.intent;
      cancelled.message    = "Superseded while queueing behind in-flight frame";
      Emit(std::move(cancelled));
      terminal_request_ids_.insert(slot->request.request_id);
      slot.reset();
    }
  }
  DeliverPendingResults();
  // Joins the scheduler worker for this session. Present waits release
  // render_lock and the port pumps GUI events so this can complete on the
  // owner thread. Coordinator inflight_ may lag one CompleteJob call; the
  // worker has already left Apply/present before Wait returns.
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
      for (auto& slot : slots_) {
        if (!slot.has_value() || slot->request.request_id != request_id) {
          continue;
        }
        EditorRenderResult cancelled;
        cancelled.kind       = EditorRenderResultKind::Cancelled;
        cancelled.request_id = request_id;
        cancelled.intent     = slot->request.intent;
        cancelled.message    = "Cancelled by request id";
        Emit(std::move(cancelled));
        terminal_request_ids_.insert(request_id);
        slot.reset();
        ScheduleNext();
        did_cancel = true;
        break;
      }
    }
    if (!did_cancel && terminal_request_ids_.count(request_id) == 0 && inflight_ &&
        inflight_->request.request_id == request_id) {
      scheduler_job_to_cancel = inflight_->scheduler_job_id;
      if (inflight_->request.intent.cancellation) {
        inflight_->request.intent.cancellation->Cancel();
      }
      EditorRenderResult cancelled;
      cancelled.kind       = EditorRenderResultKind::Cancelled;
      cancelled.request_id = request_id;
      cancelled.intent     = inflight_->request.intent;
      cancelled.message    = "Cancelled in-flight by request id";
      Emit(std::move(cancelled));
      terminal_request_ids_.insert(request_id);
      did_cancel = true;
    }
  }
  if (scheduler_ && scheduler_job_to_cancel != 0) {
    scheduler_->Cancel(scheduler_job_to_cancel);
  }
  DeliverPendingResults();
  return did_cancel;
}

void EditorRenderCoordinator::ScrubPendingSlots() {
  for (auto& slot : slots_) {
    if (!slot.has_value()) {
      continue;
    }
    if (slot->request.intent.cancellation && slot->request.intent.cancellation->IsCancelled()) {
      EditorRenderResult cancelled;
      cancelled.kind       = EditorRenderResultKind::Cancelled;
      cancelled.request_id = slot->request.request_id;
      cancelled.intent     = slot->request.intent;
      cancelled.message    = "Cancelled by token before schedule";
      Emit(std::move(cancelled));
      terminal_request_ids_.insert(slot->request.request_id);
      slot.reset();
      continue;
    }
    if (IsObsolete(slot->request.intent)) {
      EditorRenderResult cancelled;
      cancelled.kind       = EditorRenderResultKind::Cancelled;
      cancelled.request_id = slot->request.request_id;
      cancelled.intent     = slot->request.intent;
      cancelled.message    = "Cancelled: obsolete image load request before schedule";
      Emit(std::move(cancelled));
      terminal_request_ids_.insert(slot->request.request_id);
      slot.reset();
    }
  }
}

void EditorRenderCoordinator::ScheduleNext() {
  if (inflight_ || !scheduler_) {
    return;
  }

  ScrubPendingSlots();
  const auto next_slot = SelectNextSlotIndex();
  if (!next_slot.has_value()) {
    return;
  }

  PendingEntry entry = std::move(*slots_[*next_slot]);
  slots_[*next_slot].reset();

  const auto request_id = entry.request.request_id;
  // Forward completion only: adapter must not hold a reverse coordinator pointer.
  // Pool completion defers ScheduleNext (false) then Pump so present can settle.
  EditorPipelineScheduleCompletion on_complete =
      [this, request_id](bool success, std::string message) {
        NotifySchedulerCompleted(request_id, success, std::move(message),
                                 /*schedule_next_from_pool=*/false);
        Pump();
      };
  const std::uint64_t job_id = scheduler_->Schedule(entry.request, std::move(on_complete));
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
  diag::NoteRenderE2eScheduled(inflight_->request.request_id);

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
                                                       std::string message,
                                                       bool schedule_next_from_pool) {
  std::uint64_t completed_image_load_request_id = 0;
  std::vector<SessionIdleCallback> ready_callbacks;
  {
    std::scoped_lock lock(mutex_);
    if (inflight_ && inflight_->request.request_id == request_id) {
      completed_image_load_request_id =
          inflight_->request.intent.image_load_request_id.value;
      const bool already_terminal = terminal_request_ids_.count(request_id) != 0;
      const bool cancelled = inflight_->request.intent.cancellation &&
                             inflight_->request.intent.cancellation->IsCancelled();
      EditorRenderResult completed;
      completed.kind       = success     ? EditorRenderResultKind::FrameReady
                             : cancelled ? EditorRenderResultKind::Cancelled
                                         : EditorRenderResultKind::Failed;
      completed.request_id = request_id;
      completed.intent     = inflight_->request.intent;
      completed.message    = std::move(message);
      if (!already_terminal) {
        Emit(std::move(completed));
        terminal_request_ids_.insert(request_id);
      }
      inflight_.reset();
      // Defer the next ScheduleNext when the pipeline pool just handed a Ready
      // frame to present; keeps produce from overlapping the current vsync.
      if (schedule_next_from_pool) {
        ScheduleNext();
      }
      ready_callbacks = TakeIdleCallbacks(completed_image_load_request_id);
    }
  }
  DeliverPendingResults();
  for (auto& callback : ready_callbacks) {
    callback(completed_image_load_request_id);
  }
}

auto EditorRenderCoordinator::HasSessionWork(std::uint64_t image_load_request_id) const -> bool {
  if (inflight_ &&
      inflight_->request.intent.image_load_request_id.value == image_load_request_id) {
    return true;
  }
  return std::any_of(slots_.begin(), slots_.end(), [image_load_request_id](const auto& slot) {
    return slot && slot->request.intent.image_load_request_id.value == image_load_request_id;
  });
}

auto EditorRenderCoordinator::TakeIdleCallbacks(std::uint64_t image_load_request_id)
    -> std::vector<SessionIdleCallback> {
  std::vector<SessionIdleCallback> ready;
  if (HasSessionWork(image_load_request_id)) {
    return ready;
  }
  auto it = idle_callbacks_.begin();
  while (it != idle_callbacks_.end()) {
    if (it->image_load_request_id != image_load_request_id) {
      ++it;
      continue;
    }
    ready.push_back(std::move(it->callback));
    it = idle_callbacks_.erase(it);
  }
  return ready;
}

}  // namespace alcedo
