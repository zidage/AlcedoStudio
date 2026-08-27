//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_METAL

#include <cstdint>
#include <vector>

#include "edit/graph/pipeline_document.hpp"
#include "edit/input/prepared_raw_input.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/metal/metal_backend.hpp"
#include "edit/runtime/metal/metal_renderer.hpp"

namespace alcedo {

struct MetalPrimaryGradeResult {
  GraphValueId  output{NodeId{"grade.primary"}, PortId{"image"}};
  std::uint32_t pointwise_dispatch_count = 0;
  std::uint32_t detail_pass_count        = 0;
  std::uint32_t command_upload_bytes     = 0;
  std::uint64_t lut_resource_id          = 0;
};

/**
 * @brief Encode Primary Grade on the current Metal command buffer.
 *
 * Pointwise adjustments fuse into one dispatch per LLF segment. Clarity, Sharpen,
 * Halation, and Film Grain are explicit TexturePool passes. Parameters live in
 * ParameterArena. Failures throw; there is no CPU or old fused-pipeline substitute.
 */
[[nodiscard]] auto ExecuteMetalPrimaryGrade(MetalRenderDevice& device, const ExecutionPlan& plan,
                                            const PreparedRawInput& prepared,
                                            PipelineDocument& document) -> MetalPrimaryGradeResult;

void               AppendMetalPrimaryGradeWarmup(std::vector<MetalPipelineWarmup>& pipelines);

}  // namespace alcedo

#endif
