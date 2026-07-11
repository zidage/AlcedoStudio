//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "demosaicnet_perf_metrics.hpp"

#include <cuda_runtime.h>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace alcedo::perf {
namespace {

[[nodiscard]] auto AlignPercentileIndex(const std::size_t n, const double percentile) -> double {
  if (n == 0) {
    return 0.0;
  }
  const double clamped = std::clamp(percentile, 0.0, 100.0);
  return (clamped / 100.0) * static_cast<double>(n - 1);
}

}  // namespace

auto PercentileMs(std::vector<double> sorted_ms, const double percentile) -> double {
  if (sorted_ms.empty()) {
    return 0.0;
  }
  std::sort(sorted_ms.begin(), sorted_ms.end());
  const double pos   = AlignPercentileIndex(sorted_ms.size(), percentile);
  const auto   lo    = static_cast<std::size_t>(std::floor(pos));
  const auto   hi    = static_cast<std::size_t>(std::ceil(pos));
  if (lo == hi) {
    return sorted_ms[lo];
  }
  const double frac  = pos - static_cast<double>(lo);
  return sorted_ms[lo] * (1.0 - frac) + sorted_ms[hi] * frac;
}

auto ComputeTimingStats(std::vector<double> samples_ms) -> TimingStats {
  TimingStats stats;
  stats.samples_ms = std::move(samples_ms);
  stats.count      = stats.samples_ms.size();
  if (stats.count == 0) {
    return stats;
  }

  std::vector<double> sorted = stats.samples_ms;
  std::sort(sorted.begin(), sorted.end());
  stats.min_ms    = sorted.front();
  stats.max_ms    = sorted.back();
  stats.median_ms = PercentileMs(sorted, 50.0);
  stats.p90_ms    = PercentileMs(sorted, 90.0);
  stats.p95_ms    = PercentileMs(sorted, 95.0);

  double sum = 0.0;
  for (const double v : stats.samples_ms) {
    sum += v;
  }
  stats.mean_ms = sum / static_cast<double>(stats.count);

  if (stats.count > 1) {
    double var = 0.0;
    for (const double v : stats.samples_ms) {
      const double d = v - stats.mean_ms;
      var += d * d;
    }
    stats.stddev_ms = std::sqrt(var / static_cast<double>(stats.count - 1));
  }
  return stats;
}

auto QueryDeviceInfo(const int device) -> DeviceInfo {
  DeviceInfo info;
  cudaDeviceProp prop{};
  if (cudaGetDeviceProperties(&prop, device) != cudaSuccess) {
    info.name = "unknown";
    return info;
  }
  info.name                    = prop.name;
  info.compute_major           = prop.major;
  info.compute_minor           = prop.minor;
  info.multi_processor_count   = prop.multiProcessorCount;
  info.total_global_mem_bytes  = prop.totalGlobalMem;
  info.clock_rate_khz          = prop.clockRate;
  cudaDriverGetVersion(&info.driver_version);
  cudaRuntimeGetVersion(&info.runtime_version);
  return info;
}

auto FormatCudaVersion(const int version) -> std::string {
  if (version <= 0) {
    return "unknown";
  }
  const int major = version / 1000;
  const int minor = (version % 1000) / 10;
  return std::to_string(major) + "." + std::to_string(minor);
}

auto JsonEscape(const std::string_view text) -> std::string {
  std::string out;
  out.reserve(text.size() + 8);
  for (const char c : text) {
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out += c;
        break;
    }
  }
  return out;
}

auto JsonNumber(const double value, const int precision) -> std::string {
  if (!std::isfinite(value)) {
    return "null";
  }
  std::ostringstream oss;
  oss << std::setprecision(precision) << std::defaultfloat << value;
  return oss.str();
}

void AppendJsonKeyString(std::string& out, const std::string_view key, const std::string_view value,
                         const bool trailing_comma) {
  out += "\"";
  out += key;
  out += "\":\"";
  out += JsonEscape(value);
  out += trailing_comma ? "\"," : "\"";
}

void AppendJsonKeyNumber(std::string& out, const std::string_view key, const double value,
                         const bool trailing_comma, const int precision) {
  out += "\"";
  out += key;
  out += "\":";
  out += JsonNumber(value, precision);
  if (trailing_comma) {
    out += ",";
  }
}

void AppendJsonKeyInt(std::string& out, const std::string_view key, const std::int64_t value,
                      const bool trailing_comma) {
  out += "\"";
  out += key;
  out += "\":";
  out += std::to_string(value);
  if (trailing_comma) {
    out += ",";
  }
}

void AppendJsonKeyBool(std::string& out, const std::string_view key, const bool value,
                       const bool trailing_comma) {
  out += "\"";
  out += key;
  out += "\":";
  out += value ? "true" : "false";
  if (trailing_comma) {
    out += ",";
  }
}

void AppendJsonTimingStats(std::string& out, const std::string_view key, const TimingStats& stats,
                           const bool trailing_comma) {
  out += "\"";
  out += key;
  out += "\":{";
  AppendJsonKeyInt(out, "count", static_cast<std::int64_t>(stats.count));
  AppendJsonKeyNumber(out, "min_ms", stats.min_ms);
  AppendJsonKeyNumber(out, "median_ms", stats.median_ms);
  AppendJsonKeyNumber(out, "mean_ms", stats.mean_ms);
  AppendJsonKeyNumber(out, "p90_ms", stats.p90_ms);
  AppendJsonKeyNumber(out, "p95_ms", stats.p95_ms);
  AppendJsonKeyNumber(out, "max_ms", stats.max_ms);
  AppendJsonKeyNumber(out, "stddev_ms", stats.stddev_ms, false);
  out += trailing_comma ? "}," : "}";
}

void PrintTimingTable(const std::string_view title, const TimingStats& stats,
                      const double active_megapixels, const int tile_count) {
  std::cout << "\n=== " << title << " ===\n";
  if (stats.count == 0) {
    std::cout << "  (no samples)\n";
    return;
  }
  std::cout << std::fixed << std::setprecision(3);
  std::cout << "  n=" << stats.count << "  min=" << stats.min_ms << "  median=" << stats.median_ms
            << "  mean=" << stats.mean_ms << "  p90=" << stats.p90_ms << "  p95=" << stats.p95_ms
            << "  max=" << stats.max_ms << "  stddev=" << stats.stddev_ms << " ms\n";
  if (active_megapixels > 0.0) {
    std::cout << "  active MP/s (median)="
              << ActiveMegapixelsPerSecond(stats.median_ms, active_megapixels) << "\n";
  }
  if (tile_count > 0) {
    std::cout << "  tiles/s (median)=" << TilesPerSecond(stats.median_ms, tile_count) << "\n";
  }
}

auto ActiveMegapixelsPerSecond(const double median_ms, const double active_megapixels) -> double {
  if (median_ms <= 0.0 || active_megapixels <= 0.0) {
    return 0.0;
  }
  return active_megapixels / (median_ms / 1000.0);
}

auto TilesPerSecond(const double median_ms, const int tile_count) -> double {
  if (median_ms <= 0.0 || tile_count <= 0) {
    return 0.0;
  }
  return static_cast<double>(tile_count) / (median_ms / 1000.0);
}

CudaEventRange::CudaEventRange() {
  if (cudaEventCreate(&start_) != cudaSuccess || cudaEventCreate(&stop_) != cudaSuccess) {
    throw std::runtime_error("CudaEventRange: failed to create CUDA events");
  }
}

CudaEventRange::~CudaEventRange() {
  if (start_ != nullptr) {
    cudaEventDestroy(start_);
  }
  if (stop_ != nullptr) {
    cudaEventDestroy(stop_);
  }
}

void CudaEventRange::RecordStart(const cudaStream_t stream) {
  cudaEventRecord(start_, stream);
}

void CudaEventRange::RecordStop(const cudaStream_t stream) {
  cudaEventRecord(stop_, stream);
}

auto CudaEventRange::ElapsedMs() -> float {
  cudaEventSynchronize(stop_);
  float ms = 0.0F;
  cudaEventElapsedTime(&ms, start_, stop_);
  return ms;
}

}  // namespace alcedo::perf
