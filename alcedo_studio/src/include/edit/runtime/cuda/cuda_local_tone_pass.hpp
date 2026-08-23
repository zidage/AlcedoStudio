//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>

#include "edit/graph/graph_ids.hpp"

namespace alcedo {

class CudaRenderDevice;

/**
 * @brief Apply the Shadows/Highlights local-Laplacian stage to AP1/ACEScc pixels.
 *
 * Pyramid storage is owned by the render workspace and is reused by later submissions.
 * Both input and output must be RGBA32F images of @p width by @p height. The slider
 * values use the persisted [-100, 100] UI scale. Throws on missing resources or CUDA failure.
 *
 * @return Resource id of the workspace-owned level-zero LLF reference buffer.
 */
[[nodiscard]] auto ExecuteCudaLocalTone(CudaRenderDevice& device, const GraphValueId& input,
                                        const GraphValueId& output, const NodeId& grade_id,
                                        std::uint32_t width, std::uint32_t height,
                                        float shadows_slider, float highlights_slider)
    -> std::uint64_t;

}  // namespace alcedo
