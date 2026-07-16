//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "opencl/nn/common.hpp"

namespace alcedo::opencl::nn {

// Development-only profiler for fixed OpenCL DemosaicNet execution.
//
// Modes:
// - BoundaryDrain: finishes the in-order queue at each stage boundary (diagnostic only;
//   never enable for product wall timing).
// - EventTimestamps: attaches CL events without mid-network waits; after the product
//   final wait, CollectEventTimestamps() reports device/queue/host components.
//
// Product code is inert unless a scope installs an active collector.
enum class DemosaicNetProfileMode : std::uint8_t {
  BoundaryDrain = 0,
  EventTimestamps,
};

struct DemosaicNetStageTiming {
  std::string name;
  std::size_t calls = 0;
  // BoundaryDrain: host wall spanning enqueue + forced clFinish.
  // EventTimestamps: host wall spanning BeginStage..FinishStage (enqueue only).
  double host_enqueue_wall_ms = 0.0;
  // EventTimestamps only: sum of (END - START) over retained events.
  double device_exec_ms = 0.0;
  // EventTimestamps only: sum of (START - QUEUED).
  double queue_delay_ms = 0.0;
  // EventTimestamps only: sum of (SUBMIT - QUEUED).
  double submit_delay_ms = 0.0;
  // BoundaryDrain: host wall including finish. EventTimestamps: device_exec_ms.
  double total_ms = 0.0;
  std::size_t event_count = 0;
};

struct DemosaicNetProfileSummary {
  DemosaicNetProfileMode mode = DemosaicNetProfileMode::BoundaryDrain;
  double                 wall_ms                      = 0.0;
  double                 host_enqueue_wall_ms         = 0.0;
  double                 device_exec_sum_ms           = 0.0;
  double                 queue_delay_sum_ms           = 0.0;
  double                 submit_delay_sum_ms          = 0.0;
  double                 residual_wall_minus_device_ms = 0.0;
  std::size_t            event_count                  = 0;
  bool                   events_collected             = false;
  bool                   used_profiling_queue         = false;
};

class DemosaicNetStageProfiler {
 public:
  explicit DemosaicNetStageProfiler(
      DemosaicNetProfileMode mode = DemosaicNetProfileMode::BoundaryDrain);
  ~DemosaicNetStageProfiler();

  DemosaicNetStageProfiler(const DemosaicNetStageProfiler&)            = delete;
  DemosaicNetStageProfiler& operator=(const DemosaicNetStageProfiler&) = delete;

  [[nodiscard]] auto mode() const noexcept -> DemosaicNetProfileMode { return mode_; }
  [[nodiscard]] auto UsesProfilingQueue() const noexcept -> bool {
    return mode_ == DemosaicNetProfileMode::EventTimestamps;
  }

  // EventTimestamps: install OpenClContext profiling-queue override for this scope.
  void BeginSession();
  void EndSession();

  void BeginWall();
  void EndWall();

  // BoundaryDrain only: drain queue before the first stage.
  void Drain(cl_command_queue queue);

  void BeginStage(std::string_view name);
  // BoundaryDrain: finishes the queue. EventTimestamps: no wait.
  void FinishStage(std::string_view name, cl_command_queue queue);

  // EventTimestamps: retain an enqueue event under the active stage (or "untagged").
  // Ownership transfers to the profiler (released on collect/reset).
  void NoteEvent(cl_event event);
  void NoteEvent(std::string_view stage_name, cl_event event);

  // After the product final wait: query profiling timestamps. No extra clFinish.
  void CollectEventTimestamps();

  void Reset();

  [[nodiscard]] auto Timings() const -> const std::vector<DemosaicNetStageTiming>&;
  [[nodiscard]] auto Summary() const -> DemosaicNetProfileSummary;
  [[nodiscard]] auto ToJsonObjectBody() const -> std::string;

 private:
  struct StagedEvent {
    std::string stage;
    cl_event    event = nullptr;
  };

  void ReleaseEvents() noexcept;
  void AccumulateStageHost(std::string_view name, double host_ms, bool include_finish_wall);

  DemosaicNetProfileMode mode_ = DemosaicNetProfileMode::BoundaryDrain;
  bool                   session_active_ = false;
  bool                   events_collected_ = false;
  bool                   used_profiling_queue_ = false;

  std::vector<DemosaicNetStageTiming> timings_;
  std::vector<StagedEvent>            events_;

  std::string                           active_stage_;
  std::chrono::steady_clock::time_point active_start_{};

  std::chrono::steady_clock::time_point wall_start_{};
  bool                                  wall_running_ = false;
  double                                wall_ms_      = 0.0;
};

[[nodiscard]] auto ActiveDemosaicNetStageProfiler() noexcept -> DemosaicNetStageProfiler*;

class DemosaicNetStageProfilerScope {
 public:
  explicit DemosaicNetStageProfilerScope(DemosaicNetStageProfiler* profiler) noexcept;
  ~DemosaicNetStageProfilerScope();

  DemosaicNetStageProfilerScope(const DemosaicNetStageProfilerScope&) = delete;
  auto operator=(const DemosaicNetStageProfilerScope&)
      -> DemosaicNetStageProfilerScope& = delete;

 private:
  DemosaicNetStageProfiler* previous_ = nullptr;
};

void BeginDemosaicNetStage(std::string_view name);
void FinishDemosaicNetStage(std::string_view name, cl_command_queue queue);

// RAII: when EventTimestamps profiling is active, out() is non-null for clEnqueue*
// event arguments. On destruction the event is transferred to the active profiler.
class ScopedStageEvent {
 public:
  ScopedStageEvent();
  ~ScopedStageEvent();

  ScopedStageEvent(const ScopedStageEvent&)            = delete;
  ScopedStageEvent& operator=(const ScopedStageEvent&) = delete;

  [[nodiscard]] auto out() noexcept -> cl_event*;

 private:
  cl_event event_  = nullptr;
  bool     active_ = false;
};

}  // namespace alcedo::opencl::nn

#endif  // HAVE_OPENCL
