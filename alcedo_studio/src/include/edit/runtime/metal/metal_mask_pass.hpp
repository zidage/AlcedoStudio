//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_METAL

#include <cstdint>
#include <span>
#include <vector>

#include "edit/graph/pipeline_document.hpp"
#include "edit/mask/mask_store.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/metal/metal_backend.hpp"
#include "edit/runtime/metal/metal_renderer.hpp"

namespace alcedo {

struct MetalMaskResult {
  GraphValueId  output;
  std::uint64_t persistent_texture_resource_id = 0;
  std::uint64_t signed_distance_resource_id    = 0;
  std::uint32_t mip_level_count                = 0;
  std::uint32_t transient_bytes                = 0;
};

/**
 * @brief Evaluate the optional compiled analytic or raster mask into RenderSpace R8.
 *
 * Raster source textures and mip levels live in workspace MaskTextureCache. Signed-distance
 * intermediates come from TransientBufferArena. The signed-distance result is stored by mask
 * content key so a feather-radius edit can reuse it. Failures throw; there is no CPU substitute.
 */
[[nodiscard]] auto ExecuteMetalMask(MetalRenderDevice& device, const ExecutionPlan& plan,
                                    const PipelineDocument& document, MaskStore* store = nullptr,
                                    std::span<const RectI> dirty_rectangles = {})
    -> MetalMaskResult;

void AppendMetalMaskWarmup(std::vector<MetalPipelineWarmup>& pipelines);

}  // namespace alcedo

#endif
