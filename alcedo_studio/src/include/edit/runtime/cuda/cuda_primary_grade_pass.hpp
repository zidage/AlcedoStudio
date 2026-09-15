//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>

#include "edit/graph/pipeline_document.hpp"
#include "edit/input/prepared_raw_input.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/frame_scene_binding.hpp"

namespace alcedo {

struct CudaPrimaryGradeResult {
  GraphValueId      output{NodeId{""}, PortId{"image"}};
  FrameSceneBinding output_binding{};
  std::uint64_t lut_resource_id                        = 0;
  std::uint64_t local_tone_reference_resource_id       = 0;
  bool          local_tone_rebuilt_reference           = false;
  bool          local_tone_sampled_canonical_reference = false;
  std::uint32_t pointwise_dispatch_count               = 0;
  std::uint32_t detail_pass_count                      = 0;
  std::uint32_t local_tone_pass_count                  = 0;
};

/**
 * @brief Execute one compiled Color Grade in the AP1/ACEScc working space.
 *
 * Consumes @p scene as the complete Grade input. Mix reads that input, not Develop.
 * Disabled and zero-mix Grades return @p scene without a pixel copy. The physical
 * output is a scene-work member; Grade scene_output is never published.
 *
 * Must run between CudaRenderDevice::BeginRender and EndRender. Parameters, output
 * images, execution order, and local-tone reference data are owned by the device
 * workspace. A failed parameter transfer restores the affected Model dirty bits.
 * No CPU image-processing fallback.
 */
[[nodiscard]] auto ExecuteCudaPrimaryGrade(CudaRenderDevice& device, const ExecutionPlan& plan,
                                           const PreparedRawInput& prepared,
                                           PipelineDocument&       document,
                                           const CompiledGradeNode& compiled_grade,
                                           const FrameSceneBinding& scene)
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
