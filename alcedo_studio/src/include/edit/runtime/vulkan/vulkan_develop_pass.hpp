//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_VULKAN

#include "edit/graph/pipeline_document.hpp"
#include "edit/input/prepared_raw_input.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/vulkan/vulkan_render_device.hpp"

namespace alcedo {

/**
 * @brief Records SensorDevelop into `develop.sensor_linear`: the Vulkan form of
 * `ExecuteOpenClDevelop` (plan #1036, phases VK6 and VK7).
 *
 * The branches and their order are the OpenCL ones: a CFA input (upload, `ToLinearRef`,
 * `CfaClamp01` without highlight reconstruction, RCD or X-Trans, then the pack, or the highlight
 * reconstruction and the pack), a raster input (upload at native depth, `LinearizeRaster`, crop and
 * orientation, phase VK7), a `DebayeredRgb` or `DirectRgb` input (upload, `LinearizeRgb`,
 * optional highlight reconstruction, crop and orientation), then the DNG `WarpRectilinear` from
 * `develop.sensor_unwarped`, then the lens calibration when the plan has `GpuPassKind::Lens`.
 *
 * Records into the open recording of the device; does not submit or wait. The scratch buffers and
 * textures belong to the call and are released at its end; each one stays alive until the batch
 * that uses it completes. Output: RGBA32F, camera scene-linear for a RAW input, linear light in the
 * source primaries for a raster input.
 *
 * @throws std::runtime_error Before a command is recorded when BeginRender has not run, the
 *         document has no Develop node, the CFA is empty or not tightly packed, the RGB input is
 *         not tightly packed RGBA32F, the raster input is not tightly packed or does not match the
 *         raster description of the document, or the demosaic method is the Neural Engine
 *         (deferred by decision A5). No other backend and no other method runs (decision B8).
 * @throws std::runtime_error or std::invalid_argument From an allocation or an encode function.
 */
void ExecuteVulkanDevelop(VulkanRenderDevice& device, const ExecutionPlan& plan,
                          const PreparedRawInput& input, const PipelineDocument& document);

}  // namespace alcedo

#endif  // HAVE_VULKAN
