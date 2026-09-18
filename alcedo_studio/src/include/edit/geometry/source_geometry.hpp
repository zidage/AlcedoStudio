//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <stdexcept>

#include "edit/geometry/types.hpp"

namespace alcedo {

/**
 * @brief Decoded buffer size and the stable full-image reference it maps onto.
 *
 * @p sensor_active_area is recorded for Develop / CFA phase (G4). Image matrices
 * do not bake a sensor offset: decoded space is already active-area pixels.
 */
struct SourceGeometry {
  Extent2D      decoded_extent{};
  Extent2D      full_reference_extent{};
  RectI         sensor_active_area{};
  std::uint8_t  downsample_passes = 0;
  Matrix3x3     decoded_to_reference = Matrix3x3::Identity();
};

/**
 * @brief Scale matrix from decoded pixel-center coords onto full reference coords.
 *
 * Uses extent ratio, not 2^passes, so odd full sizes still share normalized UVs.
 */
[[nodiscard]] inline auto MakeDecodedToReference(Extent2D decoded, Extent2D full_reference)
    -> Matrix3x3 {
  if (decoded.Empty() || full_reference.Empty()) {
    throw std::runtime_error("MakeDecodedToReference: extents must be positive");
  }
  const float sx =
      static_cast<float>(full_reference.width) / static_cast<float>(decoded.width);
  const float sy =
      static_cast<float>(full_reference.height) / static_cast<float>(decoded.height);
  return Matrix3x3::Scale(sx, sy);
}

[[nodiscard]] inline auto MakeSourceGeometry(Extent2D decoded, Extent2D full_reference,
                                             RectI sensor_active_area = {},
                                             std::uint8_t downsample_passes = 0) -> SourceGeometry {
  SourceGeometry source;
  source.decoded_extent         = decoded;
  source.full_reference_extent  = full_reference;
  source.sensor_active_area     = sensor_active_area;
  source.downsample_passes      = downsample_passes;
  source.decoded_to_reference   = MakeDecodedToReference(decoded, full_reference);
  return source;
}

}  // namespace alcedo
