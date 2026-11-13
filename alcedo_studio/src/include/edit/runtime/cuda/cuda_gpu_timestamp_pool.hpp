//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "utils/diagnostics/preview_performance_record.hpp"

namespace alcedo {

/**
 * @brief Prebuilt CUDA timing events for preview pass and sub-stage samples.
 *
 * Records start/stop on the render stream without host waits. Elapsed times are
 * read after the submission stream has completed. In-flight pairs are never
 * recycled. Owner: CudaBackend. Not thread-safe.
 */
class CudaGpuTimestampPool {
 public:
  CudaGpuTimestampPool();
  ~CudaGpuTimestampPool();

  CudaGpuTimestampPool(const CudaGpuTimestampPool&)                    = delete;
  auto operator=(const CudaGpuTimestampPool&) -> CudaGpuTimestampPool& = delete;

  /**
   * @brief Record a start event for the current preview pass or sub-stage.
   *
   * No-op when preview timing has no open target. Grows the pool instead of
   * reusing an in-flight pair.
   */
  void Begin(cudaStream_t stream, std::uint64_t submission_id);

  /**
   * @brief Record the stop event for the innermost unmatched Begin on this stream.
   */
  void End(cudaStream_t stream);

  /**
   * @brief Read elapsed times for stop events that have already completed.
   *
   * Does not wait. Incomplete slots stay in flight for a later call.
   */
  void ResolveReady();

  /**
   * @brief Recycle every slot without publishing durations. Call after a failed encode wait.
   */
  void DiscardAll();

  [[nodiscard]] auto InFlightCount() const -> std::size_t;
  [[nodiscard]] auto SlotCount() const -> std::size_t;

 private:
  struct Slot {
    cudaEvent_t               start          = nullptr;
    cudaEvent_t               stop           = nullptr;
    std::uint64_t             request_id     = 0;
    std::uint64_t             submission_id  = 0;
    std::uint8_t              pass_index     = 0;
    std::uint8_t              sub_index      = 0;
    bool                      is_sub         = false;
    bool                      open           = false;
    bool                      recorded       = false;
    bool                      in_flight      = false;
  };

  void Grow(std::size_t extra);
  auto AcquireFreeSlot() -> std::size_t;
  void Recycle(Slot& slot) noexcept;

  std::vector<Slot>        slots_;
  std::vector<std::size_t> open_stack_;
};

}  // namespace alcedo
