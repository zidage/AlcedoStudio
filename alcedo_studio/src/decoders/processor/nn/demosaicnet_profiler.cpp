//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "decoders/processor/nn/demosaicnet_profiler.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "cuda/nn/common.hpp"

namespace alcedo {
namespace {

thread_local DemosaicNetProfiler* g_active_profiler = nullptr;

[[nodiscard]] auto ParseIntToken(const char* token) -> int {
  if (token == nullptr || token[0] == '\0' || std::strcmp(token, "[N/A]") == 0) {
    return -1;
  }
  char* end = nullptr;
  const long value = std::strtol(token, &end, 10);
  if (end == token) {
    return -1;
  }
  return static_cast<int>(value);
}

[[nodiscard]] auto ParseDoubleToken(const char* token) -> double {
  if (token == nullptr || token[0] == '\0' || std::strcmp(token, "[N/A]") == 0) {
    return -1.0;
  }
  char* end = nullptr;
  const double value = std::strtod(token, &end);
  if (end == token) {
    return -1.0;
  }
  return value;
}

void AppendJsonEscaped(std::string& out, const std::string_view text) {
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
      default:
        out += c;
        break;
    }
  }
}

void AppendJsonNumber(std::string& out, const double value) {
  if (!std::isfinite(value)) {
    out += "null";
    return;
  }
  std::ostringstream oss;
  oss.precision(6);
  oss << std::defaultfloat << value;
  out += oss.str();
}

}  // namespace

auto QueryDemosaicNetGpuTelemetry() -> DemosaicNetGpuTelemetry {
  DemosaicNetGpuTelemetry snap;
#if defined(_WIN32)
  FILE* pipe = _popen(
      "nvidia-smi --query-gpu=temperature.gpu,power.draw,clocks.sm,clocks.mem,pstate "
      "--format=csv,noheader,nounits",
      "r");
#else
  FILE* pipe = popen(
      "nvidia-smi --query-gpu=temperature.gpu,power.draw,clocks.sm,clocks.mem,pstate "
      "--format=csv,noheader,nounits",
      "r");
#endif
  if (pipe == nullptr) {
    return snap;
  }
  char line[512] = {};
  if (std::fgets(line, sizeof(line), pipe) != nullptr) {
    // temperature.gpu, power.draw, clocks.sm, clocks.mem, pstate
    // Parse CSV fields without strtok (portable across MSVC / libc).
    auto next_field = [](char*& cursor) -> char* {
      if (cursor == nullptr || *cursor == '\0') {
        return nullptr;
      }
      char* start = cursor;
      while (*cursor != '\0' && *cursor != ',' && *cursor != '\r' && *cursor != '\n') {
        ++cursor;
      }
      if (*cursor != '\0') {
        *cursor++ = '\0';
      }
      while (*start == ' ' || *start == '\t') {
        ++start;
      }
      return start;
    };
    char* cursor = line;
    if (char* tok = next_field(cursor); tok != nullptr) {
      snap.temperature_c = ParseIntToken(tok);
    }
    if (char* tok = next_field(cursor); tok != nullptr) {
      snap.power_w = ParseDoubleToken(tok);
    }
    if (char* tok = next_field(cursor); tok != nullptr) {
      snap.sm_clock_mhz = ParseIntToken(tok);
    }
    if (char* tok = next_field(cursor); tok != nullptr) {
      snap.mem_clock_mhz = ParseIntToken(tok);
    }
    if (char* tok = next_field(cursor); tok != nullptr) {
      char* end = tok + std::strlen(tok);
      while (end > tok && (end[-1] == ' ' || end[-1] == '\t')) {
        --end;
      }
      snap.pstate.assign(tok, end);
    }
    snap.available = snap.temperature_c >= 0 || snap.sm_clock_mhz >= 0 || !snap.pstate.empty();
  }
#if defined(_WIN32)
  _pclose(pipe);
#else
  pclose(pipe);
#endif
  return snap;
}

DemosaicNetProfiler::DemosaicNetProfiler() {
  open_range_.fill(static_cast<std::size_t>(-1));
  range_sum_ms_.fill(0.0);
}

DemosaicNetProfiler::~DemosaicNetProfiler() {
  for (auto& iv : intervals_) {
    if (iv.start != nullptr) {
      cudaEventDestroy(iv.start);
    }
    if (iv.stop != nullptr) {
      cudaEventDestroy(iv.stop);
    }
  }
  if (frame_start_ != nullptr) {
    cudaEventDestroy(frame_start_);
  }
  if (frame_stop_ != nullptr) {
    cudaEventDestroy(frame_stop_);
  }
}

void DemosaicNetProfiler::Reset() {
  for (auto& iv : intervals_) {
    if (iv.start != nullptr) {
      cudaEventDestroy(iv.start);
    }
    if (iv.stop != nullptr) {
      cudaEventDestroy(iv.stop);
    }
  }
  intervals_.clear();
  open_range_.fill(static_cast<std::size_t>(-1));
  open_trunk_layer_ = static_cast<std::size_t>(-1);
  open_trunk_index_ = -1;
  frame_open_       = false;
  frame_closed_     = false;
  wall_running_          = false;
  wall_ms_               = 0.0;
  batch_cuda_ms_         = 0.0;
  host_wait_running_     = false;
  host_stream_wait_ms_   = 0.0;
  range_sum_ms_.fill(0.0);
  trunk_layer_sum_ms_.clear();
  tile_count_                 = 0;
  kernel_launches_per_tile_   = 0;
  kernel_launches_per_frame_  = 0;
  activation_workspace_bytes_ = 0;
  owned_device_bytes_         = 0;
  finalized_                  = false;
}

void DemosaicNetProfiler::EnsureEvent(cudaEvent_t& event) {
  if (event == nullptr) {
    cuda::nn::CheckCuda(cudaEventCreate(&event), "DemosaicNetProfiler event create");
  }
}

auto DemosaicNetProfiler::StartInterval(const DemosaicNetProfileRange range, const int trunk_layer,
                                        const cudaStream_t stream) -> std::size_t {
  Interval iv;
  iv.range       = range;
  iv.trunk_layer = trunk_layer;
  EnsureEvent(iv.start);
  EnsureEvent(iv.stop);
  cuda::nn::CheckCuda(cudaEventRecord(iv.start, stream), "DemosaicNetProfiler start record");
  intervals_.push_back(iv);
  return intervals_.size() - 1;
}

void DemosaicNetProfiler::StopInterval(const std::size_t index, const cudaStream_t stream) {
  if (index >= intervals_.size()) {
    throw std::runtime_error("DemosaicNetProfiler: stop on invalid interval");
  }
  auto& iv = intervals_[index];
  if (iv.closed) {
    throw std::runtime_error("DemosaicNetProfiler: interval already closed");
  }
  cuda::nn::CheckCuda(cudaEventRecord(iv.stop, stream), "DemosaicNetProfiler stop record");
  iv.closed = true;
}

void DemosaicNetProfiler::BeginFrame(const cudaStream_t stream) {
  if (frame_open_) {
    throw std::runtime_error("DemosaicNetProfiler: nested BeginFrame");
  }
  EnsureEvent(frame_start_);
  EnsureEvent(frame_stop_);
  cuda::nn::CheckCuda(cudaEventRecord(frame_start_, stream), "DemosaicNetProfiler frame start");
  frame_open_    = true;
  frame_closed_  = false;
  wall_start_    = Clock::now();
  wall_running_  = true;
  finalized_     = false;
}

void DemosaicNetProfiler::EndFrame(const cudaStream_t stream) {
  if (!frame_open_ || frame_closed_) {
    throw std::runtime_error("DemosaicNetProfiler: EndFrame without open frame");
  }
  cuda::nn::CheckCuda(cudaEventRecord(frame_stop_, stream), "DemosaicNetProfiler frame stop");
  frame_closed_ = true;
  // Wall continues through optional host stream wait; closed in Finalize().
}

void DemosaicNetProfiler::BeginRange(const DemosaicNetProfileRange range,
                                     const cudaStream_t stream) {
  const auto idx = static_cast<std::size_t>(range);
  if (idx >= open_range_.size()) {
    throw std::runtime_error("DemosaicNetProfiler: invalid range");
  }
  if (open_range_[idx] != static_cast<std::size_t>(-1)) {
    throw std::runtime_error(std::string("DemosaicNetProfiler: nested range ") +
                             DemosaicNetProfileRangeName(range));
  }
  open_range_[idx] = StartInterval(range, -1, stream);
}

void DemosaicNetProfiler::EndRange(const DemosaicNetProfileRange range, const cudaStream_t stream) {
  const auto idx = static_cast<std::size_t>(range);
  if (idx >= open_range_.size() || open_range_[idx] == static_cast<std::size_t>(-1)) {
    throw std::runtime_error(std::string("DemosaicNetProfiler: EndRange without Begin for ") +
                             DemosaicNetProfileRangeName(range));
  }
  StopInterval(open_range_[idx], stream);
  open_range_[idx] = static_cast<std::size_t>(-1);
}

void DemosaicNetProfiler::BeginHostStreamWait() {
  if (host_wait_running_) {
    throw std::runtime_error("DemosaicNetProfiler: nested BeginHostStreamWait");
  }
  host_wait_start_   = Clock::now();
  host_wait_running_ = true;
}

void DemosaicNetProfiler::EndHostStreamWait() {
  if (!host_wait_running_) {
    throw std::runtime_error("DemosaicNetProfiler: EndHostStreamWait without Begin");
  }
  host_stream_wait_ms_ +=
      std::chrono::duration<double, std::milli>(Clock::now() - host_wait_start_).count();
  host_wait_running_ = false;
  // Product wall ends when the stream is drained (exclude post-process host work).
  if (wall_running_) {
    wall_ms_      = std::chrono::duration<double, std::milli>(Clock::now() - wall_start_).count();
    wall_running_ = false;
  }
}

void DemosaicNetProfiler::BeginTrunkLayer(const int layer_index, const cudaStream_t stream) {
  if (layer_index < 0) {
    throw std::runtime_error("DemosaicNetProfiler: negative trunk layer");
  }
  if (open_trunk_layer_ != static_cast<std::size_t>(-1)) {
    throw std::runtime_error("DemosaicNetProfiler: nested trunk layer");
  }
  open_trunk_layer_ = StartInterval(DemosaicNetProfileRange::Trunk, layer_index, stream);
  open_trunk_index_ = layer_index;
}

void DemosaicNetProfiler::EndTrunkLayer(const int layer_index, const cudaStream_t stream) {
  if (open_trunk_layer_ == static_cast<std::size_t>(-1) || open_trunk_index_ != layer_index) {
    throw std::runtime_error("DemosaicNetProfiler: EndTrunkLayer mismatch");
  }
  StopInterval(open_trunk_layer_, stream);
  open_trunk_layer_ = static_cast<std::size_t>(-1);
  open_trunk_index_ = -1;
}

void DemosaicNetProfiler::NoteTile() { ++tile_count_; }

void DemosaicNetProfiler::SetWorkspaceBytes(const std::size_t activation_bytes,
                                            const std::size_t owned_device_bytes) {
  activation_workspace_bytes_ = activation_bytes;
  owned_device_bytes_         = owned_device_bytes;
}

void DemosaicNetProfiler::SetKernelLaunchCounts(const int per_tile, const int per_frame) {
  kernel_launches_per_tile_  = per_tile;
  kernel_launches_per_frame_ = per_frame;
}

void DemosaicNetProfiler::CaptureTelemetryBefore() {
  telemetry_before_ = QueryDemosaicNetGpuTelemetry();
}

void DemosaicNetProfiler::CaptureTelemetryAfter() {
  telemetry_after_ = QueryDemosaicNetGpuTelemetry();
}

void DemosaicNetProfiler::Finalize() {
  if (finalized_) {
    return;
  }
  for (std::size_t i = 0; i < open_range_.size(); ++i) {
    if (open_range_[i] != static_cast<std::size_t>(-1)) {
      throw std::runtime_error(std::string("DemosaicNetProfiler: unclosed range ") +
                               DemosaicNetProfileRangeName(
                                   static_cast<DemosaicNetProfileRange>(i)));
    }
  }
  if (open_trunk_layer_ != static_cast<std::size_t>(-1)) {
    throw std::runtime_error("DemosaicNetProfiler: unclosed trunk layer");
  }
  if (frame_open_ && !frame_closed_) {
    throw std::runtime_error("DemosaicNetProfiler: frame not ended before Finalize");
  }

  range_sum_ms_.fill(0.0);
  trunk_layer_sum_ms_.clear();

  for (auto& iv : intervals_) {
    if (!iv.closed) {
      throw std::runtime_error("DemosaicNetProfiler: unclosed interval at Finalize");
    }
    cuda::nn::CheckCuda(cudaEventSynchronize(iv.stop), "DemosaicNetProfiler interval sync");
    float ms = 0.0F;
    cuda::nn::CheckCuda(cudaEventElapsedTime(&ms, iv.start, iv.stop),
                        "DemosaicNetProfiler interval elapsed");
    if (iv.trunk_layer >= 0) {
      if (static_cast<int>(trunk_layer_sum_ms_.size()) <= iv.trunk_layer) {
        trunk_layer_sum_ms_.resize(static_cast<std::size_t>(iv.trunk_layer) + 1, 0.0);
      }
      trunk_layer_sum_ms_[static_cast<std::size_t>(iv.trunk_layer)] += static_cast<double>(ms);
      // Also accumulate into bulk Trunk so decision gates see full trunk CUDA time.
      range_sum_ms_[static_cast<std::size_t>(DemosaicNetProfileRange::Trunk)] +=
          static_cast<double>(ms);
    } else {
      range_sum_ms_[static_cast<std::size_t>(iv.range)] += static_cast<double>(ms);
    }
  }

  if (frame_closed_ && frame_start_ != nullptr && frame_stop_ != nullptr) {
    cuda::nn::CheckCuda(cudaEventSynchronize(frame_stop_), "DemosaicNetProfiler frame sync");
    float ms = 0.0F;
    cuda::nn::CheckCuda(cudaEventElapsedTime(&ms, frame_start_, frame_stop_),
                        "DemosaicNetProfiler frame elapsed");
    batch_cuda_ms_ = static_cast<double>(ms);
  }

  if (wall_running_) {
    wall_ms_      = std::chrono::duration<double, std::milli>(Clock::now() - wall_start_).count();
    wall_running_ = false;
  }
  if (host_wait_running_) {
    throw std::runtime_error("DemosaicNetProfiler: unclosed host stream wait");
  }
  // Host stream-wait is not a CUDA-event interval; fold into StreamWait for reporting.
  range_sum_ms_[static_cast<std::size_t>(DemosaicNetProfileRange::StreamWait)] +=
      host_stream_wait_ms_;

  finalized_ = true;
}

auto DemosaicNetProfiler::range_sum_ms(const DemosaicNetProfileRange range) const -> double {
  const auto idx = static_cast<std::size_t>(range);
  if (idx >= range_sum_ms_.size()) {
    return 0.0;
  }
  return range_sum_ms_[idx];
}

auto DemosaicNetProfiler::sum_of_cuda_ranges_ms() const -> double {
  // Exclude host-only StreamWait from the CUDA-event sum vs batch_cuda comparison.
  double sum = 0.0;
  for (std::size_t i = 0; i < range_sum_ms_.size(); ++i) {
    if (static_cast<DemosaicNetProfileRange>(i) == DemosaicNetProfileRange::StreamWait) {
      continue;
    }
    sum += range_sum_ms_[i];
  }
  return sum;
}

auto DemosaicNetProfiler::ToJsonObjectBody() const -> std::string {
  std::string out;
  out.reserve(2048);
  out += "\"tile_count\":";
  out += std::to_string(tile_count_);
  out += ",\"kernel_launches_per_tile\":";
  out += std::to_string(kernel_launches_per_tile_);
  out += ",\"kernel_launches_per_frame\":";
  out += std::to_string(kernel_launches_per_frame_);
  out += ",\"activation_workspace_bytes\":";
  out += std::to_string(activation_workspace_bytes_);
  out += ",\"owned_device_bytes\":";
  out += std::to_string(owned_device_bytes_);
  out += ",\"wall_ms\":";
  AppendJsonNumber(out, wall_ms_);
  out += ",\"batch_cuda_ms\":";
  AppendJsonNumber(out, batch_cuda_ms_);
  out += ",\"sum_of_cuda_ranges_ms\":";
  AppendJsonNumber(out, sum_of_cuda_ranges_ms());
  if (batch_cuda_ms_ > 0.0) {
    out += ",\"sum_ranges_over_batch_cuda\":";
    AppendJsonNumber(out, sum_of_cuda_ranges_ms() / batch_cuda_ms_);
  }
  if (wall_ms_ > 0.0) {
    out += ",\"batch_cuda_over_wall\":";
    AppendJsonNumber(out, batch_cuda_ms_ / wall_ms_);
    out += ",\"wall_over_batch_cuda\":";
    AppendJsonNumber(out, wall_ms_ / std::max(batch_cuda_ms_, 1e-9));
    out += ",\"wall_minus_batch_cuda_pct\":";
    AppendJsonNumber(out, 100.0 * (wall_ms_ - batch_cuda_ms_) / wall_ms_);
  }

  out += ",\"ranges_ms\":{";
  bool first = true;
  for (std::size_t i = 0; i < static_cast<std::size_t>(DemosaicNetProfileRange::Count); ++i) {
    if (!first) {
      out += ",";
    }
    first = false;
    out += "\"";
    out += DemosaicNetProfileRangeName(static_cast<DemosaicNetProfileRange>(i));
    out += "\":";
    AppendJsonNumber(out, range_sum_ms_[i]);
  }
  out += "}";

  if (batch_cuda_ms_ > 0.0) {
    out += ",\"ranges_pct_of_batch_cuda\":{";
    first = true;
    for (std::size_t i = 0; i < static_cast<std::size_t>(DemosaicNetProfileRange::Count); ++i) {
      if (!first) {
        out += ",";
      }
      first = false;
      out += "\"";
      out += DemosaicNetProfileRangeName(static_cast<DemosaicNetProfileRange>(i));
      out += "\":";
      AppendJsonNumber(out, 100.0 * range_sum_ms_[i] / batch_cuda_ms_);
    }
    out += "}";
  }

  out += ",\"trunk_layers_ms\":[";
  for (std::size_t i = 0; i < trunk_layer_sum_ms_.size(); ++i) {
    if (i > 0) {
      out += ",";
    }
    AppendJsonNumber(out, trunk_layer_sum_ms_[i]);
  }
  out += "]";

  const double pack_unpack_roi =
      range_sum_ms(DemosaicNetProfileRange::ReflectPadPack) +
      range_sum_ms(DemosaicNetProfileRange::NchwHwcUnpack) +
      range_sum_ms(DemosaicNetProfileRange::OwnedRoiCopy);
  const double trunk_post = range_sum_ms(DemosaicNetProfileRange::Trunk) +
                            range_sum_ms(DemosaicNetProfileRange::PostOutput);
  out += ",\"pack_unpack_roi_ms\":";
  AppendJsonNumber(out, pack_unpack_roi);
  out += ",\"trunk_plus_post_ms\":";
  AppendJsonNumber(out, trunk_post);
  if (batch_cuda_ms_ > 0.0) {
    out += ",\"pack_unpack_roi_pct_of_batch_cuda\":";
    AppendJsonNumber(out, 100.0 * pack_unpack_roi / batch_cuda_ms_);
    out += ",\"trunk_plus_post_pct_of_batch_cuda\":";
    AppendJsonNumber(out, 100.0 * trunk_post / batch_cuda_ms_);
  }

  auto append_telem = [&](const char* key, const DemosaicNetGpuTelemetry& t) {
    out += ",\"";
    out += key;
    out += "\":{";
    out += "\"available\":";
    out += t.available ? "true" : "false";
    out += ",\"temperature_c\":";
    out += std::to_string(t.temperature_c);
    out += ",\"power_w\":";
    AppendJsonNumber(out, t.power_w);
    out += ",\"sm_clock_mhz\":";
    out += std::to_string(t.sm_clock_mhz);
    out += ",\"mem_clock_mhz\":";
    out += std::to_string(t.mem_clock_mhz);
    out += ",\"pstate\":\"";
    AppendJsonEscaped(out, t.pstate);
    out += "\"}";
  };
  append_telem("telemetry_before", telemetry_before_);
  append_telem("telemetry_after", telemetry_after_);

  // Decision-gate booleans (informational; not correctness assertions).
  const bool wall_launch_gap =
      wall_ms_ > 0.0 && batch_cuda_ms_ > 0.0 && ((wall_ms_ - batch_cuda_ms_) / wall_ms_) >= 0.10;
  const bool pack_heavy =
      batch_cuda_ms_ > 0.0 && (pack_unpack_roi / batch_cuda_ms_) > 0.15;
  const bool conv_dominant =
      batch_cuda_ms_ > 0.0 && (trunk_post / batch_cuda_ms_) >= 0.75;
  out += ",\"decision_gates\":{";
  out += "\"wall_exceeds_batch_cuda_by_ge_10pct\":";
  out += wall_launch_gap ? "true" : "false";
  out += ",\"pack_unpack_roi_exceeds_15pct_batch_cuda\":";
  out += pack_heavy ? "true" : "false";
  out += ",\"trunk_plus_post_ge_75pct_batch_cuda\":";
  out += conv_dominant ? "true" : "false";
  out += "}";

  return out;
}

DemosaicNetProfilerScope::DemosaicNetProfilerScope(DemosaicNetProfiler* profiler)
    : previous_(g_active_profiler) {
  g_active_profiler = profiler;
}

DemosaicNetProfilerScope::~DemosaicNetProfilerScope() { g_active_profiler = previous_; }

auto ActiveDemosaicNetProfiler() noexcept -> DemosaicNetProfiler* { return g_active_profiler; }

}  // namespace alcedo
