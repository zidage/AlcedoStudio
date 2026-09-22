//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <algorithm>
#include <optional>
#include <string>

#include "decoders/processor/nn/demosaicnet_preprocess_common.hpp"
#include "decoders/processor/nn/demosaicnet_specs.hpp"
#include "edit/geometry/types.hpp"

namespace alcedo {

/**
 * @brief Student-tiled Neural Engine rectangle in the phase-aligned lattice.
 *
 * Maps @p decode_crop from uploaded CFA coordinates through one phase shift and clamps
 * the far edge to the aligned lattice. The result can be smaller than the legacy demosaic
 * crop. It is the rectangle RawProcessor passes to DemosaicWithNeuralEngine.
 * Returns nullopt when the CFA cannot be phase-aligned or the mapped crop is empty.
 */
[[nodiscard]] inline auto BuildNeuralDevelopCrop(const RawCfaPattern& pattern, Extent2D host,
                                                 const RectI& decode_crop) -> std::optional<RectI> {
  if (host.Empty() || decode_crop.width <= 0 || decode_crop.height <= 0) {
    return std::nullopt;
  }
  const int min_spatial = pattern.kind == RawCfaKind::XTrans6x6 ? DemosaicNetXTransSpec::kMinSpatial
                                                                : DemosaicNetBayerSpec::kMinSpatial;
  std::string error;
  const auto  geometry = ComputeNeuralAlignedGeometry(
      pattern, static_cast<int>(host.width), static_cast<int>(host.height), min_spatial, &error);
  if (!geometry.has_value()) {
    return std::nullopt;
  }

  const int left   = std::clamp(decode_crop.x - geometry->shift_sx, 0, geometry->aligned_width);
  const int top    = std::clamp(decode_crop.y - geometry->shift_sy, 0, geometry->aligned_height);
  const int right  = std::clamp(decode_crop.x + decode_crop.width - geometry->shift_sx, left,
                                geometry->aligned_width);
  const int bottom = std::clamp(decode_crop.y + decode_crop.height - geometry->shift_sy, top,
                                geometry->aligned_height);
  if (right <= left || bottom <= top) {
    return std::nullopt;
  }
  return RectI{left, top, right - left, bottom - top};
}

}  // namespace alcedo
