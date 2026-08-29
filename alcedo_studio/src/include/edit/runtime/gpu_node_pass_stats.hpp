//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>

namespace alcedo {

/**
 * @brief Per-session execute/skip and source-upload counters for content-aware result cache.
 *
 * G7R.5 adds timing and allocation fields. Tests must assert these counters, not ResourceId.
 */
struct GpuNodePassStats {
  std::uint64_t sensor_develop_execute = 0;
  std::uint64_t sensor_develop_skip    = 0;
  std::uint64_t geometry_execute       = 0;
  std::uint64_t geometry_skip          = 0;
  std::uint64_t camera_color_execute   = 0;
  std::uint64_t camera_color_skip      = 0;
  std::uint64_t mask_execute           = 0;
  std::uint64_t mask_skip              = 0;
  std::uint64_t primary_grade_execute  = 0;
  std::uint64_t primary_grade_skip     = 0;
  std::uint64_t drt_execute            = 0;
  std::uint64_t drt_skip               = 0;
  std::uint64_t source_h2d_count       = 0;
  std::uint64_t source_h2d_bytes       = 0;
  std::uint64_t result_content_hits    = 0;
  std::uint64_t result_content_misses  = 0;

  void Reset() { *this = GpuNodePassStats{}; }
};

}  // namespace alcedo
