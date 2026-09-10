//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/mask/parameterized_brush_raster.hpp"

#include <memory>
#include <utility>

#include "edit/mask/brush_raster_encoding.hpp"
#include "edit/mask/brush_rasterizer.hpp"
#include "edit/mask/brush_source_geometry.hpp"

namespace alcedo {

auto RasterizeParameterizedBrushSource(const BrushMaskSource& source, Extent2D full_reference)
    -> ParameterizedBrushRaster {
  const auto raster = CanonicalBrushRasterExtent(full_reference);
  BrushRasterizer rasterizer;
  rasterizer.SetGeometry(raster, full_reference);
  rasterizer.RasterizeFull(source);
  ParameterizedBrushRaster result;
  result.descriptor.extent           = raster;
  result.descriptor.reference_bounds = CanonicalBrushReferenceBounds();
  const auto pixels                  = rasterizer.Pixels();
  result.pixels.assign(pixels.begin(), pixels.end());
  return result;
}

auto MakeParameterizedBrushActiveRaster(const NodeId& owner_node_id, const MaskId& mask_id,
                                        const BrushMaskSource& source, Extent2D full_reference,
                                        std::uint64_t content_revision,
                                        std::uint64_t session_generation)
    -> ActiveRasterMaskInput {
  auto raster = RasterizeParameterizedBrushSource(source, full_reference);
  ActiveRasterMaskInput input;
  input.owner_node_id      = owner_node_id;
  input.mask_id            = mask_id;
  input.session_generation = session_generation;
  input.content_revision   = content_revision == 0 ? 1 : content_revision;
  input.descriptor         = raster.descriptor;
  input.dirty_rectangle    = FullTexelRect(raster.descriptor.extent);
  input.pixels = std::make_shared<const std::vector<std::uint8_t>>(std::move(raster.pixels));
  return input;
}

}  // namespace alcedo
