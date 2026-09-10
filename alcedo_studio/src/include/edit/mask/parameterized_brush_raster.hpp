//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <vector>

#include "edit/geometry/types.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/mask/active_raster_mask.hpp"
#include "edit/mask/mask_asset.hpp"
#include "edit/mask/mask_id.hpp"
#include "edit/mask/mask_model.hpp"

namespace alcedo {

/**
 * @brief Canonical-grid R8 produced from a parameterized Brush source.
 *
 * Extent is @ref CanonicalBrushRasterExtent of @p full_reference. Bounds are
 * full-frame. Pixels are tightly packed. This is request/scratch raster data,
 * not a MaskStore asset or a history entry.
 */
struct ParameterizedBrushRaster {
  MaskAssetDescriptor       descriptor{};
  std::vector<std::uint8_t> pixels;
};

/**
 * @brief Rasterize @p source onto the canonical Brush grid for native Mix.
 *
 * Empty strokes write zeros. Placement is applied during dab stamping; samples
 * are not rewritten. Zoom, DPR, and Interactive output size must not be passed
 * as @p full_reference.
 *
 * @param source Parameterized Brush. Algorithm versions must be 1.
 * @param full_reference Developed full-frame extent in reference pixels.
 * @return Canonical descriptor and packed R8.
 * @throws std::runtime_error when extents are empty or the algorithm version is
 *         not 1, or a stroke fails rasterization.
 *
 * Thread: serial owner or native Mask pass on the render thread. Not thread-safe
 * across one rasterizer, but this function uses a local rasterizer.
 */
[[nodiscard]] auto RasterizeParameterizedBrushSource(const BrushMaskSource& source,
                                                     Extent2D full_reference)
    -> ParameterizedBrushRaster;

/**
 * @brief Request-owned Active raster from a parameterized Brush replay.
 *
 * @p pixels stay immutable for the lifetime of the returned input. Dirty
 * rectangle is the full canonical raster so the first upload is complete.
 * @p content_revision is the consume/owner Mask revision used to reject stale
 * GPU uploads. @p session_generation isolates authoring sessions; default 1 is
 * the live Interactive Mix slot.
 *
 * @throws std::runtime_error from @ref RasterizeParameterizedBrushSource.
 */
[[nodiscard]] auto MakeParameterizedBrushActiveRaster(const NodeId& owner_node_id,
                                                      const MaskId& mask_id,
                                                      const BrushMaskSource& source,
                                                      Extent2D full_reference,
                                                      std::uint64_t content_revision,
                                                      std::uint64_t session_generation = 1)
    -> ActiveRasterMaskInput;

}  // namespace alcedo
