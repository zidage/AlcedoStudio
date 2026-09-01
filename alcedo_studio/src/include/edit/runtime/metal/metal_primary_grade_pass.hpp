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

namespace alcedo {

struct MetalPrimaryGradeResult {
  GraphValueId  output{NodeId{""}, PortId{"image"}};
  std::uint32_t pointwise_dispatch_count               = 0;
  std::uint32_t detail_pass_count                      = 0;
  std::uint32_t command_upload_bytes                   = 0;
  std::uint64_t lut_resource_id                        = 0;
  std::uint64_t local_tone_reference_resource_id       = 0;
  bool          local_tone_rebuilt_reference           = false;
  bool          local_tone_sampled_canonical_reference = false;
  std::uint32_t local_tone_transient_bytes             = 0;
};

/**
 * @brief Encode one compiled Color Grade on the current Metal command buffer.
 *
 * Consumes @p compiled_grade.scene_input, that node's adjustments, optional mask,
 * and mix. Mix reads this Grade's scene input. Disabled and zero-mix Grades alias
 * the input onto the logical output. Parameters live in ParameterArena. Failures
 * throw; there is no CPU substitute.
 */
[[nodiscard]] auto ExecuteMetalPrimaryGrade(MetalRenderDevice& device, const ExecutionPlan& plan,
                                            const PreparedRawInput& prepared,
                                            PipelineDocument& document,
                                            const CompiledGradeNode& compiled_grade)
    -> MetalPrimaryGradeResult;

/**
 * @brief Encode every compiled Color Grade in backbone order.
 *
 * @return The last Grade result. @throws std::runtime_error when the plan has no Color Grade.
 */
[[nodiscard]] auto ExecuteMetalPrimaryGrade(MetalRenderDevice& device, const ExecutionPlan& plan,
                                            const PreparedRawInput& prepared,
                                            PipelineDocument& document) -> MetalPrimaryGradeResult;

void               AppendMetalPrimaryGradeWarmup(std::vector<MetalPipelineWarmup>& pipelines);

}  // namespace alcedo

#endif
