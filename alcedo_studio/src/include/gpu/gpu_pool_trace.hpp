//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdio>
#include <cstdlib>

namespace alcedo {

/** @brief Print pool totals when an alloc is at least this large. */
inline constexpr std::size_t kGpuPoolTraceMinAllocBytes = 16ull << 20;

struct GpuDeviceMemorySnapshot {
  std::size_t free_bytes  = 0;
  std::size_t total_bytes = 0;
  bool        valid       = false;
};

inline auto GpuPoolTraceEnvEnabled() -> bool {
  const char* enabled = std::getenv("ALCEDO_GPU_POOL_TRACE");
  return enabled != nullptr && enabled[0] != '\0' && enabled[0] != '0';
}

/** @brief Per-resource lines. Off unless ALCEDO_GPU_POOL_TRACE is set. */
inline auto GpuPoolTraceVerbose() -> bool { return GpuPoolTraceEnvEnabled(); }

inline auto ShouldTraceGpuPoolAlloc(std::size_t bytes) -> bool {
  return bytes >= kGpuPoolTraceMinAllocBytes || GpuPoolTraceEnvEnabled();
}

inline auto GpuPoolMiB(std::size_t bytes) -> double {
  return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

inline void PrintGpuDeviceMemory(const GpuDeviceMemorySnapshot& memory) {
  if (!memory.valid) {
    return;
  }
  const auto used = memory.total_bytes > memory.free_bytes ? memory.total_bytes - memory.free_bytes
                                                           : 0;
  std::fprintf(stderr, "[GPU_POOL] device free=%.1f used=%.1f total=%.1f MiB\n",
               GpuPoolMiB(memory.free_bytes), GpuPoolMiB(used), GpuPoolMiB(memory.total_bytes));
}

}  // namespace alcedo
