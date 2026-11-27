//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_VULKAN

#include <cstdint>

#include "edit/graph/pipeline_document.hpp"
#include "edit/input/prepared_raw_input.hpp"
#include "edit/runtime/curve_input_histogram.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/frame_scene_binding.hpp"
#include "edit/runtime/vulkan/vulkan_render_device.hpp"

namespace alcedo {

/// The outcome of one Color Grade on Vulkan: the fields of `OpenClPrimaryGradeResult`.
struct VulkanPrimaryGradeResult {
  GraphValueId      output{NodeId{""}, PortId{"image"}};
  FrameSceneBinding output_binding{};
  std::uint32_t     pointwise_dispatch_count               = 0;
  std::uint32_t     detail_pass_count                      = 0;
  std::uint32_t     local_tone_pass_count                  = 0;
  std::uint32_t     local_tone_transient_bytes             = 0;
  std::uint32_t     command_upload_bytes                   = 0;
  std::uint64_t     lut_resource_id                        = 0;
  std::uint64_t     local_tone_reference_resource_id       = 0;
  bool              local_tone_rebuilt_reference           = false;
  bool              local_tone_sampled_canonical_reference = false;
};

/**
 * @brief `GpuPassKind::PrimaryColorGrade` on the Vulkan backend (phase VK8): the form of
 * `ExecuteOpenClPrimaryGrade`, through the shared `GradeExecutor`.
 *
 * Encodes one compiled Color Grade from @p scene: the point stages of its schedule (White
 * Balance, Exposure, Contrast, Whites, Blacks, Curve, HLS, Saturation, Vibrance, the color wheels,
 * and the LMT LUT) into a scene-work member, then the mix with @p scene and the Grade mask. The
 * fused command offsets are a node value (`runtime.order`) that is uploaded when their topology
 * changes. The LUT comes from the LUT cache of the backend by content key
 * (`VulkanBackend::AcquireLut`); a Grade without a LUT binds the dummy LUT. A disabled or zero-mix
 * Grade aliases @p scene. Parameters are in the parameter arena.
 *
 * Records into the open recording; does not submit or wait.
 *
 * @param histogram When not null, the Grade also records the histogram of its curve input.
 * @throws std::runtime_error "ExecuteVulkanPrimaryGrade: ..." outside `BeginRender` or for a
 *         missing Grade, before a command is recorded; for a compiled mask output that is not in
 *         the workspace; and for a neighborhood stage (phase VK9) or the Local Laplacian stage
 *         (Shadows, Highlights: phase VK10), which this phase does not have.
 *         The caller cancels the render after a failure.
 */
[[nodiscard]] auto ExecuteVulkanPrimaryGrade(VulkanRenderDevice& device, const ExecutionPlan& plan,
                                             const PreparedRawInput&    prepared,
                                             const PipelineDocument&    document,
                                             const CompiledGradeNode&   compiled_grade,
                                             const FrameSceneBinding&   scene,
                                             CurveInputHistogramRecord* histogram = nullptr)
    -> VulkanPrimaryGradeResult;

/**
 * @brief Encodes every compiled Color Grade in backbone order, from `develop.image`.
 *
 * @param histogram When not null, the Grade with its `grade_id` records its curve input histogram.
 * @return The last Grade result. @throws std::runtime_error When the plan has no Color Grade.
 */
[[nodiscard]] auto ExecuteVulkanPrimaryGrade(VulkanRenderDevice& device, const ExecutionPlan& plan,
                                             const PreparedRawInput&    prepared,
                                             const PipelineDocument&    document,
                                             CurveInputHistogramRecord* histogram = nullptr)
    -> VulkanPrimaryGradeResult;

}  // namespace alcedo

#endif  // HAVE_VULKAN
