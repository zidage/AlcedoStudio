//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "opencl/opencl_api_counters.hpp"

#include <atomic>
#include <sstream>

namespace alcedo {
namespace {

std::atomic<bool> g_enabled{false};

std::atomic<std::uint64_t> g_create_buffer{0};
std::atomic<std::uint64_t> g_create_buffer_final_output{0};
std::atomic<std::uint64_t> g_create_sub_buffer{0};
std::atomic<std::uint64_t> g_create_kernel{0};
std::atomic<std::uint64_t> g_release_mem_object{0};
std::atomic<std::uint64_t> g_release_kernel{0};
std::atomic<std::uint64_t> g_h2d_bytes{0};
std::atomic<std::uint64_t> g_d2h_bytes{0};
std::atomic<std::uint64_t> g_program_builds{0};
std::atomic<std::uint64_t> g_final_waits{0};
std::atomic<std::uint64_t> g_queue_finish{0};
std::atomic<std::uint64_t> g_enqueue_ndrange{0};

template <typename AtomicT>
void Inc(AtomicT& counter, const std::uint64_t delta = 1) noexcept {
  if (!g_enabled.load(std::memory_order_relaxed)) {
    return;
  }
  counter.fetch_add(delta, std::memory_order_relaxed);
}

}  // namespace

auto OpenClApiCountersEnabled() noexcept -> bool {
  return g_enabled.load(std::memory_order_relaxed);
}

void EnableOpenClApiCounters(const bool enabled) noexcept {
  g_enabled.store(enabled, std::memory_order_relaxed);
}

void ResetOpenClApiCounters() noexcept {
  g_create_buffer.store(0, std::memory_order_relaxed);
  g_create_buffer_final_output.store(0, std::memory_order_relaxed);
  g_create_sub_buffer.store(0, std::memory_order_relaxed);
  g_create_kernel.store(0, std::memory_order_relaxed);
  g_release_mem_object.store(0, std::memory_order_relaxed);
  g_release_kernel.store(0, std::memory_order_relaxed);
  g_h2d_bytes.store(0, std::memory_order_relaxed);
  g_d2h_bytes.store(0, std::memory_order_relaxed);
  g_program_builds.store(0, std::memory_order_relaxed);
  g_final_waits.store(0, std::memory_order_relaxed);
  g_queue_finish.store(0, std::memory_order_relaxed);
  g_enqueue_ndrange.store(0, std::memory_order_relaxed);
}

auto GetOpenClApiCounters() noexcept -> OpenClApiCounters {
  return SnapshotOpenClApiCounters();
}

auto SnapshotOpenClApiCounters() noexcept -> OpenClApiCounters {
  OpenClApiCounters c;
  c.create_buffer              = g_create_buffer.load(std::memory_order_relaxed);
  c.create_buffer_final_output = g_create_buffer_final_output.load(std::memory_order_relaxed);
  c.create_sub_buffer          = g_create_sub_buffer.load(std::memory_order_relaxed);
  c.create_kernel              = g_create_kernel.load(std::memory_order_relaxed);
  c.release_mem_object         = g_release_mem_object.load(std::memory_order_relaxed);
  c.release_kernel             = g_release_kernel.load(std::memory_order_relaxed);
  c.h2d_bytes                  = g_h2d_bytes.load(std::memory_order_relaxed);
  c.d2h_bytes                  = g_d2h_bytes.load(std::memory_order_relaxed);
  c.program_builds             = g_program_builds.load(std::memory_order_relaxed);
  c.final_waits                = g_final_waits.load(std::memory_order_relaxed);
  c.queue_finish               = g_queue_finish.load(std::memory_order_relaxed);
  c.enqueue_ndrange            = g_enqueue_ndrange.load(std::memory_order_relaxed);
  return c;
}

auto DeltaOpenClApiCounters(const OpenClApiCounters& before,
                            const OpenClApiCounters& after) noexcept -> OpenClApiCounters {
  OpenClApiCounters d;
  d.create_buffer = after.create_buffer - before.create_buffer;
  d.create_buffer_final_output =
      after.create_buffer_final_output - before.create_buffer_final_output;
  d.create_sub_buffer  = after.create_sub_buffer - before.create_sub_buffer;
  d.create_kernel      = after.create_kernel - before.create_kernel;
  d.release_mem_object = after.release_mem_object - before.release_mem_object;
  d.release_kernel     = after.release_kernel - before.release_kernel;
  d.h2d_bytes          = after.h2d_bytes - before.h2d_bytes;
  d.d2h_bytes          = after.d2h_bytes - before.d2h_bytes;
  d.program_builds     = after.program_builds - before.program_builds;
  d.final_waits        = after.final_waits - before.final_waits;
  d.queue_finish       = after.queue_finish - before.queue_finish;
  d.enqueue_ndrange    = after.enqueue_ndrange - before.enqueue_ndrange;
  return d;
}

void NoteOpenClCreateBuffer() noexcept { Inc(g_create_buffer); }
void NoteOpenClCreateBufferFinalOutput() noexcept { Inc(g_create_buffer_final_output); }
void NoteOpenClCreateSubBuffer() noexcept { Inc(g_create_sub_buffer); }
void NoteOpenClCreateKernel() noexcept { Inc(g_create_kernel); }
void NoteOpenClReleaseMemObject() noexcept { Inc(g_release_mem_object); }
void NoteOpenClReleaseKernel() noexcept { Inc(g_release_kernel); }
void NoteOpenClH2DBytes(const std::uint64_t bytes) noexcept { Inc(g_h2d_bytes, bytes); }
void NoteOpenClD2HBytes(const std::uint64_t bytes) noexcept { Inc(g_d2h_bytes, bytes); }
void NoteOpenClProgramBuild() noexcept { Inc(g_program_builds); }
void NoteOpenClFinalWait() noexcept { Inc(g_final_waits); }
void NoteOpenClQueueFinish() noexcept { Inc(g_queue_finish); }
void NoteOpenClEnqueueNdRange() noexcept { Inc(g_enqueue_ndrange); }

auto FormatOpenClApiCounters(const OpenClApiCounters& c) -> std::string {
  std::ostringstream oss;
  oss << "create_buffer=" << c.create_buffer
      << " create_buffer_final_output=" << c.create_buffer_final_output
      << " create_sub_buffer=" << c.create_sub_buffer << " create_kernel=" << c.create_kernel
      << " release_mem=" << c.release_mem_object << " release_kernel=" << c.release_kernel
      << " h2d_bytes=" << c.h2d_bytes << " d2h_bytes=" << c.d2h_bytes
      << " program_builds=" << c.program_builds << " final_waits=" << c.final_waits
      << " queue_finish=" << c.queue_finish << " enqueue_ndrange=" << c.enqueue_ndrange;
  return oss.str();
}

auto OpenClApiCountersToJsonObjectBody(const OpenClApiCounters& c) -> std::string {
  std::ostringstream oss;
  oss << "\"create_buffer\":" << c.create_buffer << ","
      << "\"create_buffer_final_output\":" << c.create_buffer_final_output << ","
      << "\"create_sub_buffer\":" << c.create_sub_buffer << ","
      << "\"create_kernel\":" << c.create_kernel << ","
      << "\"release_mem_object\":" << c.release_mem_object << ","
      << "\"release_kernel\":" << c.release_kernel << ","
      << "\"h2d_bytes\":" << c.h2d_bytes << ","
      << "\"d2h_bytes\":" << c.d2h_bytes << ","
      << "\"program_builds\":" << c.program_builds << ","
      << "\"final_waits\":" << c.final_waits << ","
      << "\"queue_finish\":" << c.queue_finish << ","
      << "\"enqueue_ndrange\":" << c.enqueue_ndrange;
  return oss.str();
}

OpenClApiCounterScope::OpenClApiCounterScope(const bool enable) noexcept
    : previous_(OpenClApiCountersEnabled()) {
  EnableOpenClApiCounters(enable);
}

OpenClApiCounterScope::~OpenClApiCounterScope() { EnableOpenClApiCounters(previous_); }

}  // namespace alcedo

#endif  // HAVE_OPENCL
