//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_VULKAN

#include "decoders/dng_default_crop.hpp"
#include "edit/runtime/vulkan/vulkan_backend.hpp"

namespace alcedo::vulkan::geometry {

/**
 * @brief Records the DNG OpcodeList3 WarpRectilinear of @p source into @p destination (phase
 * VK5): the Vulkan form of `EncodeWarp` of opencl_develop_pass.cpp.
 *
 * Each output texel of R, G, and B samples the source bilinearly at the position of the radial
 * and tangential model of its plane (one coefficient set for every plane, or one set for each
 * plane); alpha samples at the green position. A position outside the source reads (0, 0, 0, 1).
 * The coefficients and the center are rounded to float, as in OpenCL.
 *
 * Records into the open recording of @p command_context; does not submit or wait.
 *
 * @throws std::invalid_argument With the values when a texture is empty or not RGBA32F, the two
 *         textures are one texture or have different sizes, or the opcode has no coefficient set
 *         or more than three. Nothing is recorded.
 */
void EncodeWarpRectilinear(VulkanBackend& backend, VulkanCommandContext& command_context,
                           const VulkanBackend::Texture2D& source,
                           VulkanBackend::Texture2D& destination, const dng::WarpRectilinear& warp);

}  // namespace alcedo::vulkan::geometry

#endif  // HAVE_VULKAN
