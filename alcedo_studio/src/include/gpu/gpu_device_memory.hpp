//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "utils/logging/log.hpp"

namespace alcedo {

/// Free and total device memory as the backend reports it. @ref valid is false when the backend
/// cannot read it.
struct GpuDeviceMemorySnapshot {
  std::size_t free_bytes  = 0;
  std::size_t total_bytes = 0;
  bool        valid       = false;
};

/// The device memory that one backend object holds now: live buffers and textures, or heap pages.
struct GpuBackendAllocationTotals {
  std::size_t   live_bytes       = 0;
  std::uint64_t live_allocations = 0;
};

/// `Backend::kName` (`CUDA`, `OpenCL`, `Metal`), or `host` for a test backend without one.
template <class Backend>
constexpr auto GpuBackendName() -> const char* {
  if constexpr (requires { Backend::kName; }) {
    return Backend::kName;
  } else {
    return "host";
  }
}

/// GPU memory key events in alcedo.gpu.pool. The backends and pools call these only when the device
/// memory state changes: an allocation failure, the first allocation, and a pool growth. Nothing is
/// written per allocation outside Debug builds.
namespace gpu_memory_log {

/// Error: a device allocation failed. Written before the backend throws its error.
inline void AllocationFailed(std::string_view backend, std::string_view operation,
                             std::size_t requested_bytes, const GpuDeviceMemorySnapshot& memory,
                             const GpuBackendAllocationTotals& totals, std::string_view error) {
  ALCEDO_LOG_ERROR(GpuPool) << "gpu.alloc.failed backend=" << backend << " op=" << operation
                            << " requested_bytes=" << requested_bytes
                            << " device_free_bytes=" << (memory.valid ? memory.free_bytes : 0)
                            << " device_total_bytes=" << (memory.valid ? memory.total_bytes : 0)
                            << " device_memory_known=" << (memory.valid ? 1 : 0)
                            << " live_bytes=" << totals.live_bytes
                            << " live_allocations=" << totals.live_allocations
                            << " error=" << error;
}

/// Info: the first device allocation of a backend object.
inline void FirstAllocation(std::string_view backend, std::string_view device,
                            std::size_t first_bytes, const GpuDeviceMemorySnapshot& memory) {
  ALCEDO_LOG_INFO(GpuPool) << "gpu.alloc.first backend=" << backend << " device=\"" << device
                           << "\" first_bytes=" << first_bytes
                           << " device_free_bytes=" << (memory.valid ? memory.free_bytes : 0)
                           << " device_total_bytes=" << (memory.valid ? memory.total_bytes : 0);
}

/// Info: a pool or heap grew to @p new_capacity bytes. @p pool names it (`transient_arena`,
/// `metal_heap`).
inline void PoolGrowth(std::string_view backend, std::string_view pool, std::string_view reason,
                       std::size_t old_capacity, std::size_t new_capacity, std::size_t slab_count) {
  ALCEDO_LOG_INFO(GpuPool) << "gpu.pool.grow backend=" << backend << " pool=" << pool
                           << " reason=" << reason << " old_capacity=" << old_capacity
                           << " new_capacity=" << new_capacity << " slabs=" << slab_count;
}

}  // namespace gpu_memory_log
}  // namespace alcedo
