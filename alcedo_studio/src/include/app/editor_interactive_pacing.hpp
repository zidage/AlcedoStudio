//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <optional>

namespace alcedo {

/**
 * @brief Interactive consume cadence: 16 ms total cycle, not a post-frame delay.
 *
 * A cycle starts when the owner begins consuming a batch and ends at owner-safe
 * completion. The next Interactive start is no earlier than both that completion
 * and start + 16 ms. A 5 ms cycle leaves at most 11 ms of wait; a 22 ms cycle
 * starts the next Interactive batch immediately when complete.
 *
 * Quality, Release, Undo, and other non-Interactive work ignore this wait after
 * ownership is free. Idle input (no prior Interactive start) waits zero.
 *
 * Does not manufacture empty frames or replay missed timer ticks.
 */
class EditorInteractivePacing {
 public:
  static constexpr std::int64_t kCycleBudgetNs = 16'000'000;

  /**
   * @brief Nanoseconds to wait before the next Interactive consume may start.
   *
   * @param now_ns Current monotonic time from the injected clock.
   * @return 0 when eligible now, otherwise remaining wait.
   */
  [[nodiscard]] auto InteractiveWaitNs(std::int64_t now_ns) const -> std::int64_t {
    if (!next_interactive_eligible_ns_.has_value()) {
      return 0;
    }
    if (now_ns >= *next_interactive_eligible_ns_) {
      return 0;
    }
    return *next_interactive_eligible_ns_ - now_ns;
  }

  /// Record the start of an Interactive consume/render cycle.
  void NoteInteractiveStart(std::int64_t now_ns) { interactive_start_ns_ = now_ns; }

  /**
   * @brief Record owner-safe completion of an Interactive cycle.
   *
   * Sets the next eligible time to max(completion, start + budget).
   */
  void NoteInteractiveComplete(std::int64_t now_ns) {
    if (interactive_start_ns_.has_value()) {
      const auto earliest = *interactive_start_ns_ + kCycleBudgetNs;
      next_interactive_eligible_ns_ = now_ns > earliest ? now_ns : earliest;
    } else {
      next_interactive_eligible_ns_ = now_ns;
    }
    interactive_start_ns_.reset();
  }

  /// Drop an Interactive start that never reached a frame (failed apply).
  void CancelInteractiveStart() { interactive_start_ns_.reset(); }

  /// Non-Interactive ownership finished: next Interactive may start immediately.
  void ResetAfterNonInteractive() {
    interactive_start_ns_.reset();
    next_interactive_eligible_ns_.reset();
  }

  [[nodiscard]] auto last_interactive_start_ns() const -> std::optional<std::int64_t> {
    return interactive_start_ns_;
  }
  [[nodiscard]] auto next_interactive_eligible_ns() const -> std::optional<std::int64_t> {
    return next_interactive_eligible_ns_;
  }

 private:
  std::optional<std::int64_t> interactive_start_ns_;
  std::optional<std::int64_t> next_interactive_eligible_ns_;
};

}  // namespace alcedo
