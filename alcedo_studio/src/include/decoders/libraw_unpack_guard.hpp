//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <libraw/libraw.h>

#include <algorithm>
#include <mutex>
#include <thread>

#if defined(_OPENMP)
#include <omp.h>
#endif

#if defined(__SANITIZE_ADDRESS__)
#define ALCEDO_LIBRAW_ASAN_ENABLED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define ALCEDO_LIBRAW_ASAN_ENABLED 1
#endif
#endif

#ifndef ALCEDO_LIBRAW_ASAN_ENABLED
#define ALCEDO_LIBRAW_ASAN_ENABLED 0
#endif

namespace alcedo {
namespace libraw_guard {

inline constexpr bool kAsanEnabled = ALCEDO_LIBRAW_ASAN_ENABLED != 0;

// ASan + OpenMP still races in LibRaw on Unix. Production Apple builds must
// keep the Nikon HE tile team; the previous 1-thread clamp made unpack match
// a serial decode (~4x slower than the Windows OpenMP path).
inline constexpr bool kForceSerialOpenMpUnpack = kAsanEnabled;

inline auto DesiredUnpackThreads() -> int {
  if constexpr (kForceSerialOpenMpUnpack) {
    return 1;
  }
  return std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
}

inline void ConfigureOpenMpRuntime() {
#if defined(_OPENMP)
  const int threads = DesiredUnpackThreads();
  omp_set_dynamic(0);
  omp_set_num_threads(threads);
#if defined(_OPENMP) && _OPENMP >= 200805
  omp_set_max_active_levels(1);
#endif
#endif
}

inline auto Unpack(LibRaw& raw_processor) -> int {
  ConfigureOpenMpRuntime();
#if defined(__APPLE__)
  // One OpenMP team at a time: HE tile decode is already memory-heavy.
  // Do not clamp that team to a single thread.
  static std::mutex unpack_mutex;
  std::lock_guard<std::mutex> lock(unpack_mutex);
#endif
  return raw_processor.unpack();
}

}  // namespace libraw_guard
}  // namespace alcedo
