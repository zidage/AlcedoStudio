//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "utils/diagnostics/preview_performance_record.hpp"

namespace alcedo::diag {

[[nodiscard]] inline auto PreviewDurationNs(const std::int64_t end_ns, const std::int64_t start_ns)
    -> std::int64_t {
  if (end_ns <= 0 || start_ns <= 0 || end_ns < start_ns) {
    return 0;
  }
  return end_ns - start_ns;
}

[[nodiscard]] auto PreviewQuantileNs(std::vector<std::int64_t> values, double fraction)
    -> std::int64_t;
[[nodiscard]] auto PreviewMaxNs(const std::vector<std::int64_t>& values) -> std::int64_t;
[[nodiscard]] auto FormatPreviewMilliseconds(std::int64_t nanoseconds) -> std::string;
[[nodiscard]] auto FormatPreviewMegabytes(std::size_t bytes) -> std::string;
[[nodiscard]] auto FormatPreviewLogPrefix() -> std::string;
[[nodiscard]] auto FormatPreviewSlowest(const PreviewRequestRecord& record) -> std::string;

struct PreviewWindowAccum {
  std::vector<std::int64_t> e2e_ns;
  std::vector<std::int64_t> input_ns;
  std::vector<std::int64_t> qml_ns;
  std::vector<std::int64_t> apply_ns;
  std::vector<std::int64_t> encode_ns;
  std::vector<std::int64_t> wait_ns;
  std::vector<std::int64_t> extra_schedule_ns;
  std::vector<std::int64_t> ready_to_gui_ns;
  std::vector<std::int64_t> gui_to_import_ns;
  std::vector<std::int64_t> import_to_swap_ns;
  std::uint64_t presented = 0;
  std::uint64_t dropped   = 0;
  std::uint64_t cancelled = 0;
  std::uint64_t failed    = 0;
  std::uint64_t coalesced = 0;
  std::uint64_t stale     = 0;
  std::optional<PreviewRequestRecord> slowest;
  std::int64_t slowest_e2e_ns = -1;
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  bool has_samples = false;

  void Reset() {
    *this = PreviewWindowAccum{};
    start = std::chrono::steady_clock::now();
  }

  void AddPresentedIntervals(const PreviewRequestRecord& record);
};

[[nodiscard]] auto FormatPreviewWindow(const PreviewWindowAccum& window, std::uint64_t lost)
    -> std::string;

}  // namespace alcedo::diag
