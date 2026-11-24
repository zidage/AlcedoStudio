//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_VULKAN

#include "edit/geometry/types.hpp"
#include "edit/runtime/vulkan/vulkan_backend.hpp"

namespace alcedo::vulkan::geometry {

/**
 * @brief Records the bilinear resize of @p crop of @p source into all of @p destination (phase
 * VK3).
 *
 * The Vulkan form of `OpenCL::Geometry::ResizeLinear` and of `OpenCL::Geometry::CropResize` with
 * `ResizeDownsampleAlgorithm::Bilinear`: a bilinear sample of the crop at the center of each
 * destination texel, 0 outside the source. The full source at its own size is a texture copy. The
 * size of @p destination is the output size; the caller allocates it (a pass encoder does not
 * allocate).
 *
 * Records into the open recording of @p command_context; does not submit or wait.
 *
 * @throws std::invalid_argument With the values when a texture is empty or not RGBA32F, the two
 *         textures are one texture, or @p crop is empty or not inside @p source. Nothing is
 *         recorded.
 */
void EncodeCropResize(VulkanBackend& backend, VulkanCommandContext& command_context,
                      const VulkanBackend::Texture2D& source, VulkanBackend::Texture2D& destination,
                      RectI crop);

}  // namespace alcedo::vulkan::geometry

#endif  // HAVE_VULKAN
