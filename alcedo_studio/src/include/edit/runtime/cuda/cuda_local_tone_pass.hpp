//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>

#include "edit/geometry/resolved_render_geometry.hpp"
#include "edit/graph/graph_ids.hpp"

namespace alcedo {

class CudaRenderDevice;

struct CudaLocalToneResult {
  std::uint64_t reference_resource_id          = 0;
  bool          rebuilt_reference              = false;
  bool          sampled_canonical_reference    = false;
};

/**
 * @brief Apply the Shadows/Highlights local-Laplacian stage to AP1/ACEScc pixels.
 *
 * The canonical LLF reference covers full ReferenceSpace and does not include the
 * viewport ROI. A full-EditSpace frame seeds or upgrades that reference. A viewport
 * ROI samples it through MakeLlfSamplingPlan instead of rebuilding the pyramids.
 *
 * Source versus result reuse is decided by @ref RuntimeInvalidationState. Pyramid
 * scratch is owned by the render workspace. Both input and output must be
 * RGBA32F images of @p width by @p height. Slider values use the persisted [-100, 100]
 * UI scale. Throws on missing resources, invalid geometry, or CUDA failure.
 */
[[nodiscard]] auto ExecuteCudaLocalTone(CudaRenderDevice& device, const GraphValueId& input,
                                        const GraphValueId& output, const NodeId& grade_id,
                                        std::uint32_t width, std::uint32_t height,
                                        float shadows_slider, float highlights_slider,
                                        const ResolvedRenderGeometry& geometry)
    -> CudaLocalToneResult;

}  // namespace alcedo
