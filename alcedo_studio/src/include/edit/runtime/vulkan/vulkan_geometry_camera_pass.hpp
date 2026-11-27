//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_VULKAN

#include "edit/graph/pipeline_document.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/vulkan/vulkan_render_device.hpp"

namespace alcedo {

/**
 * @brief `GpuPassKind::GeometryResample` on the Vulkan backend (plan #1036, phase VK7): the form
 * of `ExecuteOpenClGeometryResample`.
 *
 * Without a resample in @p plan (`encode_geometry_resample` false), `geometry.scene_source`
 * becomes an alias of `develop.sensor_linear` and nothing is recorded. Else one dispatch samples
 * `develop.sensor_linear` (bilinear, or bicubic for the bicubic filter) at `render_to_decoded`
 * of each texel center of the render extent, with the border color outside the decoded image.
 *
 * Records into the open recording; does not submit or wait.
 *
 * @throws std::runtime_error "ExecuteVulkanGeometryResample: ..." outside `BeginRender` or
 *         without `develop.sensor_linear`, before a command is recorded.
 */
void ExecuteVulkanGeometryResample(VulkanRenderDevice& device, const ExecutionPlan& plan);

/**
 * @brief `GpuPassKind::CameraToAp1` on the Vulkan backend (phase VK7): the form of
 * `ExecuteOpenClCameraColor`.
 *
 * Writes `develop.image` (ACEScc AP1) from `geometry.scene_source`: the `camera_to_ap1` matrix of
 * the resolved color transform (parameter arena slot `camera_color`), the DNG profile tables of
 * the effective camera profile (node value cache, `UploadDngProfileGpuData`), the ACES reference
 * gamut compression, and the ACEScc encoding. Alpha is kept.
 *
 * Records the parameter uploads and one dispatch into the open recording; does not submit or
 * wait.
 *
 * @throws std::runtime_error "ExecuteVulkanCameraColor: ..." outside `BeginRender`, without a
 *         Develop node, when the color transform does not resolve (the message of
 *         `ColorTransformFailureMessage`), or without `geometry.scene_source`, before a command is
 *         recorded.
 */
void ExecuteVulkanCameraColor(VulkanRenderDevice& device, const ExecutionPlan& plan,
                              const PipelineDocument& document);

}  // namespace alcedo

#endif  // HAVE_VULKAN
