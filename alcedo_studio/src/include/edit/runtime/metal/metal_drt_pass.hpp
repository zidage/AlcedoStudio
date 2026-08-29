//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_METAL

#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/metal/metal_backend.hpp"

namespace alcedo {

struct MetalDrtResult {
  GraphValueId output{NodeId{"drt"}, PortId{"display"}};
};

/**
 * @brief Encode ACES 2.0 or OpenDRT on the current Metal command buffer.
 *
 * Input is the compiled primary-grade AP1/ACEScc image. The pass decodes ACEScc
 * to AP1 scene-linear, runs the selected display transform, and writes a workspace
 * RGBA32F display texture. Parameters live in ParameterArena. Failures throw; there
 * is no CPU or fused-pipeline substitute.
 */
[[nodiscard]] auto ExecuteMetalDrt(MetalRenderDevice& device, const ExecutionPlan& plan,
                                   PipelineDocument& document) -> MetalDrtResult;

void               AppendMetalDrtWarmup(std::vector<MetalPipelineWarmup>& pipelines);

}  // namespace alcedo

#endif
