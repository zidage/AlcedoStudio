//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

/// Test-only monotonic clock. Tests stamp enqueue / apply / completion events
/// and advance time explicitly. Wall-clock sleeps are not used as ordering proof.

#include <atomic>
#include <cstdint>

namespace alcedo::test {

class ManualMonotonicClock {
 public:
  using nanoseconds = std::int64_t;

  [[nodiscard]] auto now_ns() const -> nanoseconds {
    return now_ns_.load(std::memory_order_acquire);
  }

  void advance_ns(nanoseconds delta) {
    now_ns_.fetch_add(delta, std::memory_order_acq_rel);
  }

  void set_ns(nanoseconds value) { now_ns_.store(value, std::memory_order_release); }

 private:
  std::atomic<nanoseconds> now_ns_{0};
};

}  // namespace alcedo::test
