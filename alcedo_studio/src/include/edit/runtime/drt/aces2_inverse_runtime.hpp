//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Host tables and parameters of the raster input conversion to the ACEScc AP1 working space
// (docs/roadmap/alcedo_studio/edit/raster_image_input_plan.md, section 5). The display-referred
// branch is the OpenColorIO 2.5.1 ACES 2.0 output transform inverse; its tables are a port of
// OCIO's ACES2 init functions and do not share parameter types with the forward DRT.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "edit/runtime/display_to_ap1_math.h"

namespace alcedo {

/**
 * @brief Immutable ACES 2.0 inverse parameters for one source gamut and peak luminance.
 *
 * `packed_` holds ALCEDO_D2A_PACKED_SIZE floats in the layout of display_to_ap1_math.h. Each
 * render backend uploads it verbatim, once per runtime instance.
 */
struct Aces2InverseRuntime {
  /// Source primaries and white that the runtime was built for.
  std::array<float, 8> source_primaries_xy_{};
  /// Limiting primaries: the source primaries, or AP1 when a source primary lies outside AP1.
  std::array<float, 8> limiting_primaries_xy_{};
  float                peak_luminance_nits_ = 100.0f;
  bool                 limiting_is_ap1_     = false;
  /// Binary-search window of the hue table around the nominal position (OCIO
  /// determine_hue_linearity_search_range).
  std::array<int, 2>   hue_linearity_search_range_{};
  std::vector<float>   packed_;

  /// Reach M per table entry (ALCEDO_D2A_TABLE_SIZE values).
  [[nodiscard]] auto   ReachMTable() const -> std::span<const float>;
  /// Hue of each table entry in degrees.
  [[nodiscard]] auto   HueTable() const -> std::span<const float>;
  /// Interleaved cusp J, cusp M and upper hull gamma inverse (3 * ALCEDO_D2A_TABLE_SIZE).
  [[nodiscard]] auto   CuspTable() const -> std::span<const float>;
};

/// Build the runtime without the cache. Used by the cache and by the table tests.
auto BuildAces2InverseRuntime(const std::array<float, 8>& source_primaries_xy,
                              float peak_luminance_nits) -> Aces2InverseRuntime;

/**
 * @brief Return the runtime for @p source_primaries_xy and @p peak_luminance_nits.
 *
 * A process-wide cache keyed by the bit patterns of the inputs builds each runtime once.
 * Thread-safe. The returned pointer stays valid for its owners; its identity changes only when
 * the key changes, which the backends use to upload the packed block once.
 */
auto ResolveAces2InverseRuntime(const std::array<float, 8>& source_primaries_xy,
                                float                       peak_luminance_nits)
    -> std::shared_ptr<const Aces2InverseRuntime>;

/// Number of runtimes that ResolveAces2InverseRuntime has built in this process.
auto Aces2InverseRuntimeBuildCount() -> std::uint64_t;

/// True when every source primary lies inside the AP1 triangle (tolerance 1e-4).
auto SourcePrimariesInsideAp1(const std::array<float, 8>& source_primaries_xy) -> bool;

/**
 * @brief Pack the scene-linear branch: one matrix from source RGB (native white) to AP1 with a
 * CAT02 adaptation to the AP1 white. The result holds ALCEDO_D2A_SCENE_PACKED_SIZE floats.
 */
auto PackSceneLinearToAp1(const std::array<float, 8>& source_primaries_xy)
    -> std::array<float, ALCEDO_D2A_SCENE_PACKED_SIZE>;

}  // namespace alcedo
