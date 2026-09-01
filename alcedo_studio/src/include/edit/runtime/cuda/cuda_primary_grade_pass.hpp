//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>

#include "edit/graph/pipeline_document.hpp"
#include "edit/input/prepared_raw_input.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/execution_plan.hpp"

namespace alcedo {

struct CudaPrimaryGradeResult {
  GraphValueId  output{NodeId{""}, PortId{"image"}};
  std::uint64_t lut_resource_id                        = 0;
  std::uint64_t local_tone_reference_resource_id       = 0;
  bool          local_tone_rebuilt_reference           = false;
  bool          local_tone_sampled_canonical_reference = false;
};

/**
 * @brief Execute one compiled Color Grade in the AP1/ACEScc working space.
 *
 * Consumes @p compiled_grade.scene_input, that node's adjustments, optional mask,
 * and mix. Mix reads this Grade's scene input, not Develop. Disabled and zero-mix
 * Grades alias the input onto the logical output without a pixel copy.
 *
 * Must run between CudaRenderDevice::BeginRender and EndRender. Parameters, output
 * images, execution order, and local-tone reference data are owned by the device
 * workspace. A failed parameter transfer restores the affected Model dirty bits.
 * No CPU image-processing fallback.
 */
[[nodiscard]] auto ExecuteCudaPrimaryGrade(CudaRenderDevice& device, const ExecutionPlan& plan,
                                           const PreparedRawInput& prepared,
                                           PipelineDocument&       document,
                                           const CompiledGradeNode& compiled_grade)
    -> CudaPrimaryGradeResult;

/**
 * @brief Execute every compiled Color Grade in backbone order.
 *
 * @return The last Grade result. @throws std::runtime_error when the plan has no Color Grade.
 */
[[nodiscard]] auto ExecuteCudaPrimaryGrade(CudaRenderDevice& device, const ExecutionPlan& plan,
                                           const PreparedRawInput& prepared,
                                           PipelineDocument&       document)
    -> CudaPrimaryGradeResult;

}  // namespace alcedo
