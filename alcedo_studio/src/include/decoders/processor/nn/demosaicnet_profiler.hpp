//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cuda_runtime.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace alcedo {

// Named non-overlapping CUDA-event ranges for Phase P0 full-frame decomposition.
// Product hot path only records when an ActiveDemosaicNetProfiler() is installed
// (benchmark / harness only). Correctness tests must not assert on these timings.
enum class DemosaicNetProfileRange : std::uint8_t {
  PhaseCropLinear = 0,          // to-linear + clamp + phase crop / CFA prep
  ReflectPadPack,               // fused reflect + sparse NCHW pack
  PackConv,                     // model pack convolution
  Trunk,                        // all trunk layers (bulk)
  ResidualUnpackCropConcat,     // residual 1x1 + transpose unpack + crop + concat
  PostOutput,                   // post conv + output conv (+ export crop)
  NchwHwcUnpack,                // NCHW → HWC
  OwnedRoiCopy,                 // first-writer owned ROI D2D copy
  StreamWait,                   // final product stream wait
  Count
};

[[nodiscard]] constexpr auto DemosaicNetProfileRangeName(const DemosaicNetProfileRange range)
    -> const char* {
  switch (range) {
    case DemosaicNetProfileRange::PhaseCropLinear:
      return "phase_crop_linear";
    case DemosaicNetProfileRange::ReflectPadPack:
      return "reflect_pad_pack";
    case DemosaicNetProfileRange::PackConv:
      return "pack_conv";
    case DemosaicNetProfileRange::Trunk:
      return "trunk";
    case DemosaicNetProfileRange::ResidualUnpackCropConcat:
      return "residual_unpack_crop_concat";
    case DemosaicNetProfileRange::PostOutput:
      return "post_output";
    case DemosaicNetProfileRange::NchwHwcUnpack:
      return "nchw_hwc_unpack";
    case DemosaicNetProfileRange::OwnedRoiCopy:
      return "owned_roi_copy";
    case DemosaicNetProfileRange::StreamWait:
      return "stream_wait";
    case DemosaicNetProfileRange::Count:
      break;
  }
  return "unknown";
}

// Static topology launch counts for one student export tile (product 1K path).
// Includes entry pack + model ops + optional HWC unpack + owned ROI copy.
// P4-A fused tail collapses post + output + export_crop into one launch and
// writes HWC directly (no NCHW unpack).
[[nodiscard]] constexpr auto StudentTileKernelLaunchCount(const bool is_xtrans,
                                                          const bool fused_tail = true) -> int {
  const int depth = is_xtrans ? 4 : 8;
  if (fused_tail) {
    // Model: pack + trunk(d) + residual + transpose + crop + concat + fused_post_output
    // Entry: reflect_pack; product: roi copy. No nchw_unpack.
    const int model = 1 + depth + 1 + 1 + 1 + 1 + 1;
    return model + 1 + 1;
  }
  // Model: pack + trunk(d) + residual + transpose + crop + concat + post + output + export_crop
  // Entry: reflect_pack + nchw_unpack; product: roi copy.
  const int model = 1 + depth + 1 + 1 + 1 + 1 + 1 + 1 + 1;
  return model + 2 + 1;
}

struct DemosaicNetGpuTelemetry {
  int         temperature_c     = -1;
  double      power_w           = -1.0;
  int         sm_clock_mhz      = -1;
  int         mem_clock_mhz     = -1;
  std::string pstate;
  bool        available         = false;
};

// Best-effort host query via nvidia-smi (no NVML link dependency).
[[nodiscard]] auto QueryDemosaicNetGpuTelemetry() -> DemosaicNetGpuTelemetry;

// Accumulates non-overlapping CUDA-event intervals across a full ProcessCudaTiled
// (or single-tile) pass. Finalize() synchronizes stop events once after stream wait.
class DemosaicNetProfiler {
 public:
  DemosaicNetProfiler();
  ~DemosaicNetProfiler();

  DemosaicNetProfiler(const DemosaicNetProfiler&)            = delete;
  DemosaicNetProfiler& operator=(const DemosaicNetProfiler&) = delete;

  void Reset();

  // Wall clock + overall CUDA span for one profiled frame/tile pass.
  void BeginFrame(cudaStream_t stream);
  void EndFrame(cudaStream_t stream);

  void BeginRange(DemosaicNetProfileRange range, cudaStream_t stream);
  void EndRange(DemosaicNetProfileRange range, cudaStream_t stream);

  // Host-side product stream wait (CUDA events cannot observe host blocking).
  // Accumulates into StreamWait range sum; do not also BeginRange(StreamWait).
  void BeginHostStreamWait();
  void EndHostStreamWait();

  // Optional nested detail under Trunk (not added again into sum_of_ranges).
  void BeginTrunkLayer(int layer_index, cudaStream_t stream);
  void EndTrunkLayer(int layer_index, cudaStream_t stream);

  void NoteTile();
  void SetWorkspaceBytes(std::size_t activation_bytes, std::size_t owned_device_bytes);
  void SetKernelLaunchCounts(int per_tile, int per_frame);

  void CaptureTelemetryBefore();
  void CaptureTelemetryAfter();

  // Synchronize events and compute milliseconds. Call after product stream wait.
  void Finalize();

  [[nodiscard]] auto finalized() const noexcept -> bool { return finalized_; }
  [[nodiscard]] auto tile_count() const noexcept -> int { return tile_count_; }
  [[nodiscard]] auto range_sum_ms(DemosaicNetProfileRange range) const -> double;
  [[nodiscard]] auto sum_of_cuda_ranges_ms() const -> double;
  [[nodiscard]] auto batch_cuda_ms() const noexcept -> double { return batch_cuda_ms_; }
  [[nodiscard]] auto wall_ms() const noexcept -> double { return wall_ms_; }
  [[nodiscard]] auto stream_wait_ms() const -> double {
    return range_sum_ms(DemosaicNetProfileRange::StreamWait);
  }
  [[nodiscard]] auto trunk_layer_sum_ms() const -> const std::vector<double>& {
    return trunk_layer_sum_ms_;
  }
  [[nodiscard]] auto activation_workspace_bytes() const noexcept -> std::size_t {
    return activation_workspace_bytes_;
  }
  [[nodiscard]] auto owned_device_bytes() const noexcept -> std::size_t {
    return owned_device_bytes_;
  }
  [[nodiscard]] auto kernel_launches_per_tile() const noexcept -> int {
    return kernel_launches_per_tile_;
  }
  [[nodiscard]] auto kernel_launches_per_frame() const noexcept -> int {
    return kernel_launches_per_frame_;
  }
  [[nodiscard]] auto telemetry_before() const -> const DemosaicNetGpuTelemetry& {
    return telemetry_before_;
  }
  [[nodiscard]] auto telemetry_after() const -> const DemosaicNetGpuTelemetry& {
    return telemetry_after_;
  }

  // JSON object body (no outer braces) for harness embedding.
  [[nodiscard]] auto ToJsonObjectBody() const -> std::string;

 private:
  struct Interval {
    cudaEvent_t               start        = nullptr;
    cudaEvent_t               stop         = nullptr;
    DemosaicNetProfileRange   range        = DemosaicNetProfileRange::Count;
    int                       trunk_layer  = -1;
    bool                      closed       = false;
  };

  void EnsureEvent(cudaEvent_t& event);
  auto StartInterval(DemosaicNetProfileRange range, int trunk_layer, cudaStream_t stream)
      -> std::size_t;
  void StopInterval(std::size_t index, cudaStream_t stream);

  std::vector<Interval>                        intervals_;
  std::array<std::size_t, static_cast<std::size_t>(DemosaicNetProfileRange::Count)> open_range_{};
  std::size_t                                  open_trunk_layer_ = static_cast<std::size_t>(-1);
  int                                          open_trunk_index_ = -1;

  cudaEvent_t                                  frame_start_ = nullptr;
  cudaEvent_t                                  frame_stop_  = nullptr;
  bool                                         frame_open_  = false;
  bool                                         frame_closed_ = false;

  using Clock = std::chrono::steady_clock;
  Clock::time_point                            wall_start_{};
  bool                                         wall_running_ = false;
  double                                       wall_ms_      = 0.0;
  double                                       batch_cuda_ms_ = 0.0;
  Clock::time_point                            host_wait_start_{};
  bool                                         host_wait_running_ = false;
  double                                       host_stream_wait_ms_ = 0.0;

  std::array<double, static_cast<std::size_t>(DemosaicNetProfileRange::Count)> range_sum_ms_{};
  std::vector<double>                          trunk_layer_sum_ms_;

  int                                          tile_count_                 = 0;
  int                                          kernel_launches_per_tile_   = 0;
  int                                          kernel_launches_per_frame_  = 0;
  std::size_t                                  activation_workspace_bytes_ = 0;
  std::size_t                                  owned_device_bytes_         = 0;

  DemosaicNetGpuTelemetry                      telemetry_before_{};
  DemosaicNetGpuTelemetry                      telemetry_after_{};
  bool                                         finalized_ = false;
};

// Installs/clears the thread-local active profiler used by product instrumentation.
class DemosaicNetProfilerScope {
 public:
  explicit DemosaicNetProfilerScope(DemosaicNetProfiler* profiler);
  ~DemosaicNetProfilerScope();

  DemosaicNetProfilerScope(const DemosaicNetProfilerScope&)            = delete;
  DemosaicNetProfilerScope& operator=(const DemosaicNetProfilerScope&) = delete;

 private:
  DemosaicNetProfiler* previous_ = nullptr;
};

[[nodiscard]] auto ActiveDemosaicNetProfiler() noexcept -> DemosaicNetProfiler*;

// RAII range helper for product code paths.
class DemosaicNetProfileRangeGuard {
 public:
  DemosaicNetProfileRangeGuard(DemosaicNetProfileRange range, cudaStream_t stream)
      : profiler_(ActiveDemosaicNetProfiler()), range_(range), stream_(stream) {
    if (profiler_ != nullptr) {
      profiler_->BeginRange(range_, stream_);
    }
  }
  ~DemosaicNetProfileRangeGuard() {
    if (profiler_ != nullptr) {
      profiler_->EndRange(range_, stream_);
    }
  }

  DemosaicNetProfileRangeGuard(const DemosaicNetProfileRangeGuard&)            = delete;
  DemosaicNetProfileRangeGuard& operator=(const DemosaicNetProfileRangeGuard&) = delete;

 private:
  DemosaicNetProfiler*      profiler_ = nullptr;
  DemosaicNetProfileRange   range_;
  cudaStream_t              stream_   = nullptr;
};

}  // namespace alcedo
