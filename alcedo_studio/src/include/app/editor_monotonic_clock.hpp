//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <chrono>
#include <cstdint>

namespace alcedo {

/**
 * @brief Monotonic nanosecond clock for Interactive consume pacing.
 *
 * Production uses the steady clock. Tests inject a manual clock so 5/16/22 ms
 * cycles are proven by timestamp, not wall-clock sleeps.
 *
 * @thread_safety Implementations used by the session owner must be safe to read
 *                from that owner thread. The steady clock is thread-safe.
 */
class IEditorMonotonicClock {
 public:
  virtual ~IEditorMonotonicClock() = default;

  /// Monotonic time in nanoseconds. Epoch is implementation-defined.
  [[nodiscard]] virtual auto NowNs() const -> std::int64_t = 0;
};

/// Production clock. Uses `std::chrono::steady_clock`.
class SteadyEditorMonotonicClock final : public IEditorMonotonicClock {
 public:
  [[nodiscard]] auto NowNs() const -> std::int64_t override {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }
};

}  // namespace alcedo
