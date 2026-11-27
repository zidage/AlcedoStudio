//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_VULKAN

#include <cstddef>
#include <cstdint>

#include "edit/runtime/vulkan/vulkan_backend.hpp"

namespace alcedo {

/**
 * @brief A byte offset into a `VulkanBackend::Buffer`: the Vulkan form of `OpenClBufferView`.
 *
 * A kernel reads and writes the buffer through its device address plus the offset (plan section
 * 4.2). The view does not own the buffer; the buffer outlives every recording that uses the view.
 */
struct VulkanBufferView {
  VulkanBackend::Buffer* buffer       = nullptr;
  std::uint32_t          offset_bytes = 0;
};

/**
 * @brief The device address of @p bytes bytes at @p view, after a check of the range.
 *
 * @param alignment The element size: the offset is a multiple of it (1, 2, 4, 8, or 16).
 * @param operation The name of the encode function, for the message.
 * @param label The name of the view, for the message.
 * @throws std::invalid_argument With @p operation, @p label, and the values when the view has no
 *         buffer, the offset is not a multiple of @p alignment, or the range is outside the buffer.
 *         Nothing is recorded.
 */
[[nodiscard]] auto RequireBufferRange(const VulkanBufferView& view, std::size_t bytes,
                                      std::size_t alignment, const char* operation,
                                      const char* label) -> VkDeviceAddress;

}  // namespace alcedo

#endif  // HAVE_VULKAN
