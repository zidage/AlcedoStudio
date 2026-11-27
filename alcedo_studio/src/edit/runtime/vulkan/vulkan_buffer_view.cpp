//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_VULKAN

#include "edit/runtime/vulkan/vulkan_buffer_view.hpp"

#include <stdexcept>
#include <string>

namespace alcedo {

auto RequireBufferRange(const VulkanBufferView& view, std::size_t bytes, std::size_t alignment,
                        const char* operation, const char* label) -> VkDeviceAddress {
  const std::string prefix = std::string(operation) + ": " + label;
  if (view.buffer == nullptr || view.buffer->Empty()) {
    throw std::invalid_argument(prefix + " has no buffer");
  }
  if (alignment == 0 || view.offset_bytes % alignment != 0) {
    throw std::invalid_argument(prefix + " offset " + std::to_string(view.offset_bytes) +
                                " is not a multiple of " + std::to_string(alignment));
  }
  const std::size_t end = static_cast<std::size_t>(view.offset_bytes) + bytes;
  if (end > view.buffer->Bytes()) {
    throw std::invalid_argument(prefix + " range of " + std::to_string(bytes) +
                                " bytes at offset " + std::to_string(view.offset_bytes) +
                                " is outside the buffer of " +
                                std::to_string(view.buffer->Bytes()) + " bytes");
  }
  return view.buffer->DeviceAddress() + view.offset_bytes;
}

}  // namespace alcedo

#endif  // HAVE_VULKAN
