//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

namespace alcedo::dng {

/**
 * @brief DNG OpcodeList3 WarpRectilinear opcode (DNG 1.3 opcode 1).
 *
 * Each coefficient set is kr0, kr1, kr2, kr3, kt0, kt1 for one plane, or for all planes when the
 * set count is 1. A point at normalized radius r (1 is the farthest image corner from the
 * center) moves to r * (kr0 + kr1 r^2 + kr2 r^4 + kr3 r^6), plus a tangential term of kt0, kt1.
 */
struct WarpRectilinear {
  uint32_t                             coefficient_set_count = 0;
  std::array<std::array<double, 6>, 3> coefficient_sets      = {};
  double                               center_x              = 0.5;
  double                               center_y              = 0.5;
};

/// What a @ref WarpRectilinear corrects.
struct WarpCorrections {
  /// The reference plane moves: lens distortion. The warp then owns the geometry.
  bool distortion = false;
  /// Another plane moves differently from the reference plane: lateral chromatic aberration.
  bool lateral_ca = false;
};

namespace detail {

/// Least movement, as a share of the distance from the center to the farthest corner, that
/// counts as a correction: half a pixel on a 45 MP image.
constexpr double kWarpCorrectionTolerance = 1.0e-4;

/// Largest movement between the points that coefficient sets @p a and @p b map a point to.
inline auto LargestWarpDifference(const std::array<double, 6>& a, const std::array<double, 6>& b)
    -> double {
  constexpr int kSamples = 32;
  double        largest  = 0.0;
  for (int i = 1; i <= kSamples; ++i) {
    const double r     = static_cast<double>(i) / kSamples;
    const double r2    = r * r;
    const auto   scale = [r2](const std::array<double, 6>& k) {
      return k[0] + k[1] * r2 + k[2] * r2 * r2 + k[3] * r2 * r2 * r2;
    };
    largest = std::max(largest, std::fabs(r * (scale(a) - scale(b))));
  }
  // The tangential term moves a point within the unit circle by at most 3 (|kt0| + |kt1|).
  return largest + 3.0 * (std::fabs(a[4] - b[4]) + std::fabs(a[5] - b[5]));
}

}  // namespace detail

/**
 * @brief What @p warp corrects.
 *
 * The reference plane is green of three planes, else the only plane. A reference plane that moves
 * a point corrects distortion; another plane that moves a point differently corrects lateral
 * chromatic aberration. Camera Raw writes a warp with an identity green plane when a lens
 * profile outside the file corrects the distortion.
 */
inline auto CorrectionsOf(const WarpRectilinear& warp) -> WarpCorrections {
  constexpr std::array<double, 6> kIdentity = {1.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  const uint32_t                  count     = warp.coefficient_set_count;
  if (count == 0 || count > warp.coefficient_sets.size()) {
    return {};
  }
  const uint32_t  reference = count == 3 ? 1 : 0;
  const auto&     sets      = warp.coefficient_sets;
  WarpCorrections corrections;
  corrections.distortion =
      detail::LargestWarpDifference(sets[reference], kIdentity) > detail::kWarpCorrectionTolerance;
  for (uint32_t set = 0; set < count; ++set) {
    if (set != reference && detail::LargestWarpDifference(sets[set], sets[reference]) >
                                detail::kWarpCorrectionTolerance) {
      corrections.lateral_ca = true;
    }
  }
  return corrections;
}

/// What @p warp corrects; nothing without a warp.
inline auto CorrectionsOf(const std::optional<WarpRectilinear>& warp) -> WarpCorrections {
  return warp.has_value() ? CorrectionsOf(*warp) : WarpCorrections{};
}

}  // namespace alcedo::dng
