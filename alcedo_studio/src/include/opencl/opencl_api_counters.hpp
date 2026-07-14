//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include <cstdint>
#include <string>

namespace alcedo {

// Development-only OpenCL API lifecycle counters for Neural telemetry.
// Product code may call Note* freely: they are no-ops while disabled (default).
// Enable only around harness/cold-hot measurement scopes.
struct OpenClApiCounters {
  std::uint64_t create_buffer              = 0;  // Neural/scratch clCreateBuffer
  std::uint64_t create_buffer_final_output = 0;  // caller-owned product destination only
  std::uint64_t create_sub_buffer          = 0;
  std::uint64_t create_kernel              = 0;
  std::uint64_t release_mem_object         = 0;  // last-reference releases we own
  std::uint64_t release_kernel             = 0;
  std::uint64_t h2d_bytes                  = 0;
  std::uint64_t d2h_bytes                  = 0;
  std::uint64_t program_builds             = 0;
  std::uint64_t final_waits                = 0;  // product-style WaitQueue / stage-end finish
  std::uint64_t queue_finish               = 0;  // every clFinish observed through helpers
  std::uint64_t enqueue_ndrange            = 0;
};

[[nodiscard]] auto OpenClApiCountersEnabled() noexcept -> bool;
void               EnableOpenClApiCounters(bool enabled) noexcept;
void               ResetOpenClApiCounters() noexcept;
[[nodiscard]] auto GetOpenClApiCounters() noexcept -> OpenClApiCounters;
[[nodiscard]] auto SnapshotOpenClApiCounters() noexcept -> OpenClApiCounters;
[[nodiscard]] auto DeltaOpenClApiCounters(const OpenClApiCounters& before,
                                          const OpenClApiCounters& after) noexcept
    -> OpenClApiCounters;

void NoteOpenClCreateBuffer() noexcept;
void NoteOpenClCreateBufferFinalOutput() noexcept;
void NoteOpenClCreateSubBuffer() noexcept;
void NoteOpenClCreateKernel() noexcept;
void NoteOpenClReleaseMemObject() noexcept;
void NoteOpenClReleaseKernel() noexcept;
void NoteOpenClH2DBytes(std::uint64_t bytes) noexcept;
void NoteOpenClD2HBytes(std::uint64_t bytes) noexcept;
void NoteOpenClProgramBuild() noexcept;
void NoteOpenClFinalWait() noexcept;
void NoteOpenClQueueFinish() noexcept;
void NoteOpenClEnqueueNdRange() noexcept;

[[nodiscard]] auto FormatOpenClApiCounters(const OpenClApiCounters& c) -> std::string;
[[nodiscard]] auto OpenClApiCountersToJsonObjectBody(const OpenClApiCounters& c) -> std::string;

// RAII enable for a harness scope. Restores previous enable flag; does not reset counts.
class OpenClApiCounterScope {
 public:
  explicit OpenClApiCounterScope(bool enable = true) noexcept;
  ~OpenClApiCounterScope();

  OpenClApiCounterScope(const OpenClApiCounterScope&)            = delete;
  OpenClApiCounterScope& operator=(const OpenClApiCounterScope&) = delete;

 private:
  bool previous_ = false;
};

}  // namespace alcedo

#endif  // HAVE_OPENCL
