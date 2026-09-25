//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Process memory reading for the library build memory tests and the search benchmark
// (library_search_and_project_size_plan.md, Phase S6).

#pragma once

#include <cstdint>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// windows.h first: psapi.h uses its types.
#include <psapi.h>
#endif

namespace alcedo::process_memory_test {

/// Private bytes committed by this process (Windows `PrivateUsage`: heap, DuckDB buffers, and
/// every other private allocation, resident or paged out). Returns -1 on other platforms.
inline auto PrivateBytes() -> int64_t {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS_EX counters{};
  if (!GetProcessMemoryInfo(GetCurrentProcess(),
                            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                            sizeof(counters))) {
    return -1;
  }
  return static_cast<int64_t>(counters.PrivateUsage);
#else
  return -1;
#endif
}

inline auto Mebibytes(int64_t bytes) -> double {
  return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

}  // namespace alcedo::process_memory_test
