//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_VULKAN

#include <cstdint>

#include "edit/graph/pipeline_document.hpp"
#include "edit/input/prepared_raw_input.hpp"
#include "edit/runtime/display_to_ap1_output.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/vulkan/vulkan_buffer_view.hpp"
#include "edit/runtime/vulkan/vulkan_render_device.hpp"

namespace alcedo {

/**
 * @brief Records `LinearizeRaster` (plan #1036, phase VK7): the Vulkan form of
 * `EnqueueOpenClLinearizeRaster`.
 *
 * Reads @p width x @p height tightly packed RGBA samples of @p format at @p source (U8 and U16
 * integer codes, or F32), applies the `PackRasterLinearize` block at @p parameters
 * (`raster_linearize_math.h`), and writes RGBA32F values with alpha 1 at @p destination.
 *
 * Records into the open recording of @p command_context; does not submit or wait.
 *
 * @param parameter_floats The float count of the block at @p parameters.
 * @throws std::invalid_argument With the values when @p format is `U16Cfa`, a view has no buffer,
 *         an offset is not a multiple of 4, or a range is outside its buffer. Nothing is recorded.
 */
void EncodeVulkanLinearizeRaster(VulkanBackend& backend, VulkanCommandContext& command_context,
                                 const VulkanBufferView& source, HostPixelFormat format,
                                 std::uint32_t width, std::uint32_t height,
                                 const VulkanBufferView& parameters, std::size_t parameter_floats,
                                 const VulkanBufferView& destination);

/**
 * @brief Records the DisplayToAp1 kernel of @p output_kind over @p source into @p destination
 * (phase VK7): the Vulkan form of `EnqueueOpenClDisplayToAp1`.
 *
 * `AcesccAp1`: `D2aSourceToAcesccAp1` of each texel (the `develop.image` encoding). `LinearAp0`:
 * the ACES 2.0 inverse alone (`A2rDisplayToAp0`), for the OpenColorIO check. Alpha is kept. The
 * block (`display_to_ap1_math.h` layout) is read at @p parameters plus @p parameter_offset.
 *
 * Records into the open recording of @p command_context; does not submit or wait.
 *
 * @param parameter_floats The float count of the block.
 * @throws std::invalid_argument With the values when a texture is empty or not RGBA32F, the two
 *         textures have different sizes or are one texture, or the block is outside
 *         @p parameters. Nothing is recorded.
 */
void EncodeVulkanDisplayToAp1(VulkanBackend& backend, VulkanCommandContext& command_context,
                              const VulkanBackend::Texture2D& source,
                              VulkanBackend::Texture2D&       destination,
                              const VulkanBackend::Buffer&    parameters,
                              std::uint32_t parameter_offset, std::size_t parameter_floats,
                              DisplayToAp1Output output_kind);

/**
 * @brief `GpuPassKind::DisplayToAp1` on the Vulkan backend (phase VK7): the form of
 * `ExecuteOpenClDisplayToAp1`.
 *
 * Writes `develop.image` (ACEScc AP1) from `geometry.scene_source` of a raster input with the
 * block of `ResolveDisplayToAp1Block` (the ACES 2.0 inverse for display-referred input, the
 * scene-linear matrix otherwise) in the parameter arena slot `display_to_ap1`.
 *
 * @throws std::runtime_error "ExecuteVulkanDisplayToAp1: ..." outside `BeginRender`, without a
 *         raster input, when the pixels do not match the description, or without
 *         `geometry.scene_source`, before a command is recorded.
 */
void ExecuteVulkanDisplayToAp1(VulkanRenderDevice& device, const ExecutionPlan& plan,
                               const PreparedRawInput& input, const PipelineDocument& document);

}  // namespace alcedo

#endif  // HAVE_VULKAN
