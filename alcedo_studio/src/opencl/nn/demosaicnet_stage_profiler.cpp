//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "opencl/nn/demosaicnet_stage_profiler.hpp"

#ifdef HAVE_OPENCL

#include <sstream>
#include <stdexcept>
#include <utility>

#include "opencl/opencl_api_counters.hpp"
#include "opencl/opencl_context.hpp"

namespace alcedo::opencl::nn {
namespace {

thread_local DemosaicNetStageProfiler* g_active_profiler = nullptr;

[[nodiscard]] auto NsToMs(const cl_ulong ns) -> double {
  return static_cast<double>(ns) / 1.0e6;
}

[[nodiscard]] auto QueryProfiling(const cl_event event, const cl_profiling_info info) -> cl_ulong {
  cl_ulong value = 0;
  CheckOpenCl(clGetEventProfilingInfo(event, info, sizeof(value), &value, nullptr),
              "DemosaicNetStageProfiler profiling query");
  return value;
}

}  // namespace

DemosaicNetStageProfiler::DemosaicNetStageProfiler(const DemosaicNetProfileMode mode)
    : mode_(mode) {}

DemosaicNetStageProfiler::~DemosaicNetStageProfiler() {
  EndSession();
  ReleaseEvents();
}

void DemosaicNetStageProfiler::BeginSession() {
  if (session_active_) {
    throw std::runtime_error("DemosaicNetStageProfiler: session already active");
  }
  Reset();
  if (mode_ == DemosaicNetProfileMode::EventTimestamps) {
    auto& ctx = OpenClContext::Instance();
    if (!ctx.IsInitialized()) {
      ctx.Initialize();
    }
    ctx.InstallProfilingQueueOverride();
    used_profiling_queue_ = true;
  }
  session_active_ = true;
}

void DemosaicNetStageProfiler::EndSession() {
  if (!session_active_) {
    return;
  }
  if (used_profiling_queue_) {
    OpenClContext::Instance().ClearQueueOverride();
    used_profiling_queue_ = false;
  }
  session_active_ = false;
}

void DemosaicNetStageProfiler::BeginWall() {
  wall_start_   = std::chrono::steady_clock::now();
  wall_running_ = true;
}

void DemosaicNetStageProfiler::EndWall() {
  if (!wall_running_) {
    return;
  }
  wall_ms_ =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wall_start_)
          .count();
  wall_running_ = false;
}

void DemosaicNetStageProfiler::Drain(const cl_command_queue queue) {
  if (mode_ != DemosaicNetProfileMode::BoundaryDrain) {
    return;
  }
  if (queue == nullptr) {
    throw std::runtime_error("DemosaicNetStageProfiler: null queue");
  }
  CheckOpenCl(clFinish(queue), "DemosaicNetStageProfiler initial queue drain");
  NoteOpenClQueueFinish();
}

void DemosaicNetStageProfiler::BeginStage(const std::string_view name) {
  if (!active_stage_.empty()) {
    throw std::runtime_error("DemosaicNetStageProfiler: stage already active: " + active_stage_);
  }
  active_stage_ = name;
  active_start_ = std::chrono::steady_clock::now();
}

void DemosaicNetStageProfiler::FinishStage(const std::string_view name,
                                           const cl_command_queue queue) {
  if (active_stage_ != name) {
    throw std::runtime_error("DemosaicNetStageProfiler: stage finish does not match active stage");
  }
  if (mode_ == DemosaicNetProfileMode::BoundaryDrain) {
    if (queue == nullptr) {
      throw std::runtime_error("DemosaicNetStageProfiler: null queue");
    }
    CheckOpenCl(clFinish(queue), "DemosaicNetStageProfiler stage queue finish");
    NoteOpenClQueueFinish();
  }
  const double elapsed_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - active_start_)
          .count();
  active_stage_.clear();
  AccumulateStageHost(name, elapsed_ms, mode_ == DemosaicNetProfileMode::BoundaryDrain);
}

void DemosaicNetStageProfiler::NoteEvent(const cl_event event) {
  if (event == nullptr) {
    return;
  }
  const std::string stage = active_stage_.empty() ? std::string("untagged") : active_stage_;
  NoteEvent(stage, event);
}

void DemosaicNetStageProfiler::NoteEvent(const std::string_view stage_name, const cl_event event) {
  if (event == nullptr || mode_ != DemosaicNetProfileMode::EventTimestamps) {
    if (event != nullptr) {
      clReleaseEvent(event);
    }
    return;
  }
  events_.push_back(StagedEvent{std::string(stage_name), event});
}

void DemosaicNetStageProfiler::CollectEventTimestamps() {
  if (mode_ != DemosaicNetProfileMode::EventTimestamps) {
    return;
  }
  if (events_collected_) {
    return;
  }

  // Events are complete after the product final wait; do not clFinish here.
  for (const auto& se : events_) {
    if (se.event == nullptr) {
      continue;
    }
    const cl_ulong queued = QueryProfiling(se.event, CL_PROFILING_COMMAND_QUEUED);
    const cl_ulong submit = QueryProfiling(se.event, CL_PROFILING_COMMAND_SUBMIT);
    const cl_ulong start  = QueryProfiling(se.event, CL_PROFILING_COMMAND_START);
    const cl_ulong end    = QueryProfiling(se.event, CL_PROFILING_COMMAND_END);

    const double device_ms = end >= start ? NsToMs(end - start) : 0.0;
    const double queue_ms  = start >= queued ? NsToMs(start - queued) : 0.0;
    const double submit_ms = submit >= queued ? NsToMs(submit - queued) : 0.0;

    DemosaicNetStageTiming* slot = nullptr;
    for (auto& timing : timings_) {
      if (timing.name == se.stage) {
        slot = &timing;
        break;
      }
    }
    if (slot == nullptr) {
      timings_.push_back({});
      slot       = &timings_.back();
      slot->name = se.stage;
    }
    slot->device_exec_ms += device_ms;
    slot->queue_delay_ms += queue_ms;
    slot->submit_delay_ms += submit_ms;
    slot->total_ms = slot->device_exec_ms;
    ++slot->event_count;
  }

  events_collected_ = true;
  ReleaseEvents();
}

void DemosaicNetStageProfiler::Reset() {
  ReleaseEvents();
  timings_.clear();
  events_collected_ = false;
  active_stage_.clear();
  wall_running_ = false;
  wall_ms_      = 0.0;
}

void DemosaicNetStageProfiler::ReleaseEvents() noexcept {
  for (auto& se : events_) {
    if (se.event != nullptr) {
      clReleaseEvent(se.event);
      se.event = nullptr;
    }
  }
  events_.clear();
}

void DemosaicNetStageProfiler::AccumulateStageHost(const std::string_view name,
                                                   const double host_ms,
                                                   const bool include_finish_wall) {
  for (auto& timing : timings_) {
    if (timing.name == name) {
      timing.host_enqueue_wall_ms += host_ms;
      if (include_finish_wall) {
        timing.total_ms += host_ms;
      }
      ++timing.calls;
      return;
    }
  }
  DemosaicNetStageTiming t;
  t.name                 = std::string(name);
  t.host_enqueue_wall_ms = host_ms;
  t.total_ms             = include_finish_wall ? host_ms : 0.0;
  t.calls                = 1;
  timings_.push_back(std::move(t));
}

auto DemosaicNetStageProfiler::Timings() const -> const std::vector<DemosaicNetStageTiming>& {
  return timings_;
}

auto DemosaicNetStageProfiler::Summary() const -> DemosaicNetProfileSummary {
  DemosaicNetProfileSummary s;
  s.mode                 = mode_;
  s.wall_ms              = wall_ms_;
  s.events_collected     = events_collected_;
  s.used_profiling_queue = used_profiling_queue_ ||
                           (mode_ == DemosaicNetProfileMode::EventTimestamps);
  for (const auto& t : timings_) {
    s.host_enqueue_wall_ms += t.host_enqueue_wall_ms;
    s.device_exec_sum_ms += t.device_exec_ms;
    s.queue_delay_sum_ms += t.queue_delay_ms;
    s.submit_delay_sum_ms += t.submit_delay_ms;
    s.event_count += t.event_count;
  }
  if (mode_ == DemosaicNetProfileMode::EventTimestamps) {
    s.residual_wall_minus_device_ms = wall_ms_ - s.device_exec_sum_ms;
  } else {
    s.residual_wall_minus_device_ms = wall_ms_ - s.host_enqueue_wall_ms;
  }
  return s;
}

auto DemosaicNetStageProfiler::ToJsonObjectBody() const -> std::string {
  const auto         summary = Summary();
  std::ostringstream oss;
  oss << std::fixed;
  oss.precision(4);
  oss << "\"mode\":\""
      << (mode_ == DemosaicNetProfileMode::EventTimestamps ? "event_timestamps" : "boundary_drain")
      << "\","
      << "\"wall_ms\":" << summary.wall_ms << ","
      << "\"host_enqueue_wall_ms\":" << summary.host_enqueue_wall_ms << ","
      << "\"device_exec_sum_ms\":" << summary.device_exec_sum_ms << ","
      << "\"queue_delay_sum_ms\":" << summary.queue_delay_sum_ms << ","
      << "\"submit_delay_sum_ms\":" << summary.submit_delay_sum_ms << ","
      << "\"residual_wall_minus_device_ms\":" << summary.residual_wall_minus_device_ms << ","
      << "\"event_count\":" << summary.event_count << ","
      << "\"events_collected\":" << (summary.events_collected ? "true" : "false") << ","
      << "\"used_profiling_queue\":" << (summary.used_profiling_queue ? "true" : "false") << ","
      << "\"stages\":[";
  for (std::size_t i = 0; i < timings_.size(); ++i) {
    const auto& t = timings_[i];
    if (i != 0) {
      oss << ",";
    }
    oss << "{\"name\":\"" << t.name << "\","
        << "\"calls\":" << t.calls << ","
        << "\"host_enqueue_wall_ms\":" << t.host_enqueue_wall_ms << ","
        << "\"device_exec_ms\":" << t.device_exec_ms << ","
        << "\"queue_delay_ms\":" << t.queue_delay_ms << ","
        << "\"submit_delay_ms\":" << t.submit_delay_ms << ","
        << "\"total_ms\":" << t.total_ms << ","
        << "\"event_count\":" << t.event_count << "}";
  }
  oss << "]";
  return oss.str();
}

auto ActiveDemosaicNetStageProfiler() noexcept -> DemosaicNetStageProfiler* {
  return g_active_profiler;
}

DemosaicNetStageProfilerScope::DemosaicNetStageProfilerScope(
    DemosaicNetStageProfiler* const profiler) noexcept
    : previous_(g_active_profiler) {
  g_active_profiler = profiler;
  if (profiler != nullptr) {
    profiler->BeginSession();
  }
}

DemosaicNetStageProfilerScope::~DemosaicNetStageProfilerScope() {
  if (g_active_profiler != nullptr) {
    g_active_profiler->EndSession();
  }
  g_active_profiler = previous_;
}

void BeginDemosaicNetStage(const std::string_view name) {
  if (auto* const profiler = ActiveDemosaicNetStageProfiler(); profiler != nullptr) {
    profiler->BeginStage(name);
  }
}

void FinishDemosaicNetStage(const std::string_view name, const cl_command_queue queue) {
  if (auto* const profiler = ActiveDemosaicNetStageProfiler(); profiler != nullptr) {
    profiler->FinishStage(name, queue);
  }
}

ScopedStageEvent::ScopedStageEvent() {
  auto* const profiler = ActiveDemosaicNetStageProfiler();
  active_ = profiler != nullptr && profiler->mode() == DemosaicNetProfileMode::EventTimestamps;
}

ScopedStageEvent::~ScopedStageEvent() {
  if (event_ == nullptr) {
    return;
  }
  if (auto* const profiler = ActiveDemosaicNetStageProfiler(); profiler != nullptr) {
    profiler->NoteEvent(event_);
  } else {
    clReleaseEvent(event_);
  }
  event_ = nullptr;
}

auto ScopedStageEvent::out() noexcept -> cl_event* { return active_ ? &event_ : nullptr; }

}  // namespace alcedo::opencl::nn

#endif  // HAVE_OPENCL
