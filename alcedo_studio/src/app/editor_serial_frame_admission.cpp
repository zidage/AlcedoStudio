//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_serial_frame_admission.hpp"

#include <utility>

namespace alcedo {

EditorSerialFrameAdmission::EditorSerialFrameAdmission()
    : clock_(std::make_shared<SteadyEditorMonotonicClock>()) {}

void EditorSerialFrameAdmission::SetClock(std::shared_ptr<IEditorMonotonicClock> clock) {
  clock_ = clock ? std::move(clock) : std::make_shared<SteadyEditorMonotonicClock>();
}

void EditorSerialFrameAdmission::SetDeadlineHandler(DeadlineHandler handler) {
  deadline_handler_ = std::move(handler);
}

auto EditorSerialFrameAdmission::NowNs() const -> std::int64_t {
  return clock_ ? clock_->NowNs() : 0;
}

auto EditorSerialFrameAdmission::TryBeginCycle(bool interactive) -> bool {
  if (holding_ownership_) {
    return false;
  }
  if (interactive) {
    const auto wait_ns = pacing_.InteractiveWaitNs(NowNs());
    if (wait_ns > 0) {
      last_deadline_delay_ns_ = wait_ns;
      if (deadline_handler_) {
        deadline_handler_(wait_ns);
      }
      return false;
    }
  }
  holding_ownership_    = true;
  cycle_is_interactive_ = interactive;
  inflight_request_id_  = 0;
  if (interactive) {
    const auto now = NowNs();
    pacing_.NoteInteractiveStart(now);
    interactive_start_times_ns_.push_back(now);
  }
  return true;
}

void EditorSerialFrameAdmission::NoteScheduledRequest(std::uint64_t request_id) {
  inflight_request_id_ = request_id;
}

void EditorSerialFrameAdmission::AbortCycle() {
  if (!holding_ownership_) {
    return;
  }
  if (cycle_is_interactive_) {
    pacing_.CancelInteractiveStart();
  }
  holding_ownership_    = false;
  cycle_is_interactive_ = false;
  inflight_request_id_  = 0;
}

auto EditorSerialFrameAdmission::CompleteIfMatches(std::uint64_t request_id, bool published_frame)
    -> bool {
  if (!holding_ownership_) {
    return false;
  }
  if (inflight_request_id_ == 0 || request_id != inflight_request_id_) {
    return false;
  }
  const auto now         = NowNs();
  const bool interactive = cycle_is_interactive_;
  holding_ownership_     = false;
  cycle_is_interactive_  = false;
  inflight_request_id_   = 0;
  if (interactive && published_frame) {
    pacing_.NoteInteractiveComplete(now);
    interactive_complete_times_ns_.push_back(now);
  } else {
    pacing_.ResetAfterNonInteractive();
  }
  return true;
}

void EditorSerialFrameAdmission::ResetPacingAfterNonInteractiveWork() {
  pacing_.ResetAfterNonInteractive();
}

auto EditorSerialFrameAdmission::InteractiveWaitNs() const -> std::int64_t {
  return pacing_.InteractiveWaitNs(NowNs());
}

void EditorSerialFrameAdmission::RequestInteractiveDeadlineIfNeeded() {
  const auto wait_ns = InteractiveWaitNs();
  if (wait_ns <= 0 || holding_ownership_) {
    return;
  }
  last_deadline_delay_ns_ = wait_ns;
  if (deadline_handler_) {
    deadline_handler_(wait_ns);
  }
}

void EditorSerialFrameAdmission::DeferOwnerWork(DeferredOwnerWork work) {
  if (!work) {
    return;
  }
  deferred_.push(std::move(work));
}

auto EditorSerialFrameAdmission::HasDeferredOwnerWork() const -> bool {
  return !deferred_.empty();
}

auto EditorSerialFrameAdmission::TakeDeferredOwnerWork() -> DeferredOwnerWork {
  if (deferred_.empty()) {
    return {};
  }
  auto taken = std::move(deferred_.front());
  deferred_.pop();
  return taken;
}

}  // namespace alcedo
