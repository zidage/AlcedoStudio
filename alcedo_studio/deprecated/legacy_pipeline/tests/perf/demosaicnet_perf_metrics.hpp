//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace alcedo::perf {

// Aggregate wall-clock / CUDA-event samples (milliseconds).
struct TimingStats {
  std::size_t         count    = 0;
  double              min_ms   = 0.0;
  double              max_ms   = 0.0;
  double              mean_ms  = 0.0;
  double              median_ms = 0.0;
  double              p90_ms   = 0.0;
  double              p95_ms   = 0.0;
  double              stddev_ms = 0.0;
  std::vector<double> samples_ms;
};

// Compute min / mean / median / p90 / p95 / max / stddev from raw samples.
// Empty input yields a zeroed stats object with empty samples.
[[nodiscard]] auto ComputeTimingStats(std::vector<double> samples_ms) -> TimingStats;

// Percentile in [0, 100] using linear interpolation on a sorted copy.
[[nodiscard]] auto PercentileMs(std::vector<double> sorted_ms, double percentile) -> double;

struct DeviceInfo {
  std::string name;
  int         compute_major      = 0;
  int         compute_minor      = 0;
  int         multi_processor_count = 0;
  std::size_t total_global_mem_bytes = 0;
  int         driver_version     = 0;  // cudaDriverGetVersion encoding
  int         runtime_version    = 0;  // cudaRuntimeGetVersion encoding
  int         clock_rate_khz     = 0;
};

[[nodiscard]] auto QueryDeviceInfo(int device = 0) -> DeviceInfo;

// Format cuda driver/runtime version integers as "major.minor".
[[nodiscard]] auto FormatCudaVersion(int version) -> std::string;

// Minimal JSON helpers (no extra runtime dependency).
[[nodiscard]] auto JsonEscape(std::string_view text) -> std::string;
[[nodiscard]] auto JsonNumber(double value, int precision = 6) -> std::string;

void AppendJsonKeyString(std::string& out, std::string_view key, std::string_view value,
                         bool trailing_comma = true);
void AppendJsonKeyNumber(std::string& out, std::string_view key, double value,
                         bool trailing_comma = true, int precision = 6);
void AppendJsonKeyInt(std::string& out, std::string_view key, std::int64_t value,
                      bool trailing_comma = true);
void AppendJsonKeyBool(std::string& out, std::string_view key, bool value,
                       bool trailing_comma = true);
void AppendJsonTimingStats(std::string& out, std::string_view key, const TimingStats& stats,
                           bool trailing_comma = true);

// Compact console table for one timing series.
void PrintTimingTable(std::string_view title, const TimingStats& stats,
                      double active_megapixels = 0.0, int tile_count = 0);

[[nodiscard]] auto ActiveMegapixelsPerSecond(double median_ms, double active_megapixels)
    -> double;
[[nodiscard]] auto TilesPerSecond(double median_ms, int tile_count) -> double;

// Optional named CUDA-event ranges (enabled via --profile-ranges).
class CudaEventRange {
 public:
  CudaEventRange();
  ~CudaEventRange();

  CudaEventRange(const CudaEventRange&)            = delete;
  CudaEventRange& operator=(const CudaEventRange&) = delete;

  void RecordStart(cudaStream_t stream = nullptr);
  void RecordStop(cudaStream_t stream = nullptr);
  // Synchronizes the stop event and returns elapsed milliseconds.
  [[nodiscard]] auto ElapsedMs() -> float;

 private:
  cudaEvent_t start_ = nullptr;
  cudaEvent_t stop_  = nullptr;
};

}  // namespace alcedo::perf
