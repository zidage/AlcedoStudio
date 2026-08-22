//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "edit/graph/pipeline_document.hpp"
#include "edit/input/prepared_raw_input.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/execution_plan.hpp"

namespace alcedo {

/**
 * @brief Encode CUDA Develop for the current in-flight submission.
 *
 * Must be called between BeginRender and EndRender. Failures throw; there is
 * no CPU Apply fallback. Develop output is camera scene-linear RGBA32F.
 *
 * CUDA order: Linearize → (optional CFA Clamp01) → Demosaic → HighlightRecover
 * on RGB when enabled. Camera-to-AP1 is not applied.
 */
void ExecuteCudaDevelop(CudaRenderDevice& device, const ExecutionPlan& plan,
                        const PreparedRawInput& input, PipelineDocument& document);

}  // namespace alcedo
