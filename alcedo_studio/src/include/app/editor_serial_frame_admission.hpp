//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <queue>
#include <vector>

#include "app/editor_interactive_pacing.hpp"
#include "app/editor_monotonic_clock.hpp"

namespace alcedo {

/**
 * @brief One-writer admission for consume/apply/render cycles.
 *
 * Owns whether a pending-input consume currently holds live ownership, the
 * Interactive 16 ms cadence, and owner work deferred until the inflight frame
 * is owner-safe. Does not own the pending-input queue, history, or coordinator.
 *
 * @thread_safety Session owner thread only. Not internally synchronized.
 */
class EditorSerialFrameAdmission {
 public:
  using DeadlineHandler  = std::function<void(std::int64_t delay_ns)>;
  using DeferredOwnerWork = std::function<void()>;

  EditorSerialFrameAdmission();

  void SetClock(std::shared_ptr<IEditorMonotonicClock> clock);
  void SetDeadlineHandler(DeadlineHandler handler);

  [[nodiscard]] auto clock() const -> const std::shared_ptr<IEditorMonotonicClock>& {
    return clock_;
  }
  [[nodiscard]] auto NowNs() const -> std::int64_t;

  /// True between TryBeginCycle success and matching CompleteIfMatches / AbortCycle.
  [[nodiscard]] auto HoldsOwnership() const -> bool { return holding_ownership_; }

  [[nodiscard]] auto inflight_request_id() const -> std::uint64_t { return inflight_request_id_; }

  [[nodiscard]] auto cycle_is_interactive() const -> bool { return cycle_is_interactive_; }

  /**
   * @brief Try to start one consume/render cycle.
   *
   * Interactive cycles honor remaining 16 ms wait and schedule a deadline
   * wakeup when not yet eligible. Non-Interactive cycles bypass that wait.
   *
   * @return false when ownership is already held or Interactive pacing must wait.
   */
  auto TryBeginCycle(bool interactive) -> bool;

  /// Remember the coordinator request id that releases this cycle.
  void NoteScheduledRequest(std::uint64_t request_id);

  /// Failed apply/route: drop ownership without consuming the Interactive budget.
  void AbortCycle();

  /**
   * @brief Release ownership when @p request_id matches the inflight consume.
   *
   * Published Interactive cycles start the 16 ms cadence. Abandoned
   * (failed/cancelled) cycles release ownership without that wait so the next
   * batch can start as soon as the owner is free.
   *
   * @return true when this completion ended the current cycle.
   */
  auto CompleteIfMatches(std::uint64_t request_id, bool published_frame = true) -> bool;

  /// Drop Interactive cadence after Undo, Checkout, or other owner work.
  void ResetPacingAfterNonInteractiveWork();

  [[nodiscard]] auto InteractiveWaitNs() const -> std::int64_t;

  /// If Interactive work is waiting on cadence, arm the deadline handler once.
  void RequestInteractiveDeadlineIfNeeded();

  void DeferOwnerWork(DeferredOwnerWork work);
  [[nodiscard]] auto HasDeferredOwnerWork() const -> bool;
  auto               TakeDeferredOwnerWork() -> DeferredOwnerWork;

  [[nodiscard]] auto interactive_start_times_ns() const -> const std::vector<std::int64_t>& {
    return interactive_start_times_ns_;
  }
  [[nodiscard]] auto interactive_complete_times_ns() const -> const std::vector<std::int64_t>& {
    return interactive_complete_times_ns_;
  }
  [[nodiscard]] auto last_deadline_delay_ns() const -> std::int64_t {
    return last_deadline_delay_ns_;
  }

 private:
  std::shared_ptr<IEditorMonotonicClock> clock_;
  DeadlineHandler                        deadline_handler_;
  EditorInteractivePacing                pacing_;
  std::queue<DeferredOwnerWork>          deferred_;
  bool                                   holding_ownership_     = false;
  bool                                   cycle_is_interactive_  = false;
  std::uint64_t                          inflight_request_id_   = 0;
  std::int64_t                           last_deadline_delay_ns_ = 0;
  std::vector<std::int64_t>              interactive_start_times_ns_;
  std::vector<std::int64_t>              interactive_complete_times_ns_;
};

}  // namespace alcedo
