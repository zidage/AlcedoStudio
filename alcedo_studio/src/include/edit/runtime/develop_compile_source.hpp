//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>

#include "edit/geometry/types.hpp"

namespace alcedo {

enum class DevelopInputKind : std::uint8_t {
  BayerCfa  = 0,
  XTransCfa = 1,
  DirectRgb = 2,
};

/**
 * @brief GPU-free develop compile inputs. No LibRaw, no pixel buffers.
 */
struct DevelopCompileSource {
  DevelopInputKind kind                   = DevelopInputKind::BayerCfa;
  Extent2D         host_extent{};
  Extent2D         develop_output_extent{};
  Extent2D         full_reference_extent{};
  RectI            sensor_active_area{};
  std::uint8_t     downsample_passes      = 0;
};

inline auto operator==(const DevelopCompileSource& a, const DevelopCompileSource& b) -> bool {
  return a.kind == b.kind && a.host_extent == b.host_extent &&
         a.develop_output_extent == b.develop_output_extent &&
         a.full_reference_extent == b.full_reference_extent &&
         a.sensor_active_area == b.sensor_active_area &&
         a.downsample_passes == b.downsample_passes;
}

inline auto operator!=(const DevelopCompileSource& a, const DevelopCompileSource& b) -> bool {
  return !(a == b);
}

/**
 * @brief Identity of a prepared source. No write counters.
 */
struct SourceContentKey {
  std::uint64_t content_hash = 0;
};

}  // namespace alcedo
