//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_VULKAN

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

#include "edit/runtime/vulkan/vulkan_backend.hpp"
#include "vulkan/vulkan_compute_pipeline_cache.hpp"

namespace alcedo {

/// One image of a dispatch: the texture for one binding of set 0 of the kernel.
struct VulkanImageBinding {
  std::uint32_t                   binding = 0;
  const VulkanBackend::Texture2D* texture = nullptr;
};

/// The workgroup count of a dispatch on each axis.
struct VulkanWorkgroupCount {
  std::uint32_t x = 1;
  std::uint32_t y = 1;
  std::uint32_t z = 1;
};

/// The workgroup count that covers @p width x @p height with workgroups of @p group_width x
/// @p group_height.
[[nodiscard]] constexpr auto WorkgroupsCovering(std::uint32_t width, std::uint32_t height,
                                                std::uint32_t group_width,
                                                std::uint32_t group_height)
    -> VulkanWorkgroupCount {
  return {(width + group_width - 1) / group_width, (height + group_height - 1) / group_height, 1};
}

/// The bytes of the push constant struct @p value (scalar layout: the layout of the host struct).
template <class PushConstants>
[[nodiscard]] auto PushConstantBytes(const PushConstants& value) -> std::span<const std::byte> {
  static_assert(std::is_trivially_copyable_v<PushConstants>);
  return std::as_bytes(std::span<const PushConstants, 1>(&value, 1));
}

/**
 * @brief Records one dispatch of @p kernel into the open recording of @p command_context (plan
 * section 4.2: a pass encoder records and does not submit or wait).
 *
 * Gets the pipeline of @p kernel from the compute pipeline cache of the process context (a cache
 * hit after the first use), opens the recording or adds the barrier after an earlier write
 * (`VulkanBackend::BeginCommand`), moves each image to `VK_IMAGE_LAYOUT_GENERAL` at its first use,
 * pushes the storage image descriptors and the push constants, and records `vkCmdDispatch`. The
 * dispatch counts as a write, so the next command of the recording starts with a memory barrier.
 *
 * A workgroup count of zero on an axis records nothing.
 *
 * @param images One entry for each binding of @p kernel, with the same binding numbers.
 * @param push_constants Exactly `kernel.push_constant_bytes` bytes.
 * @throws std::invalid_argument When the images do not match the bindings of @p kernel (count,
 *         binding number, or an empty texture) or the push constant size differs. Nothing is
 *         recorded and no pipeline is requested.
 * @throws std::runtime_error Or `VulkanError` from the pipeline cache, with the kernel name.
 */
void RecordKernelDispatch(VulkanBackend& backend, VulkanCommandContext& command_context,
                          const vulkan::VulkanKernelDescriptor& kernel,
                          std::span<const VulkanImageBinding>   images,
                          std::span<const std::byte> push_constants, VulkanWorkgroupCount groups);

}  // namespace alcedo

#endif  // HAVE_VULKAN
