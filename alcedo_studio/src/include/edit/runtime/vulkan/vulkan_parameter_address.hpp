//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_VULKAN

#include <cstddef>
#include <stdexcept>
#include <string>

#include "edit/runtime/parameter_arena.hpp"
#include "edit/runtime/vulkan/vulkan_backend.hpp"

namespace alcedo {

/**
 * @brief The device address of the parameter slot @p key of @p arena, for a kernel that reads
 * @p bytes bytes of it (plan #1036, phase VK7: a kernel reads the arena through the device address
 * of its buffer plus the binding offset, as OpenCL reads it at the offset).
 *
 * @pre The slot is bound and `UploadDirty` has recorded its upload into the open recording.
 * @throws std::invalid_argument With @p operation and the values when the arena has no device
 *         buffer, the binding is smaller than @p bytes, or the range is outside the buffer.
 */
[[nodiscard]] inline auto ParameterSlotAddress(const ParameterArena<VulkanBackend>& arena,
                                               const ParameterSlotKey& key, std::size_t bytes,
                                               const char* operation) -> VkDeviceAddress {
  const auto& buffer  = arena.DeviceBuffer();
  const auto& binding = arena.Binding(key);
  if (buffer.Empty()) {
    throw std::invalid_argument(std::string(operation) + ": the parameter arena has no buffer");
  }
  if (binding.size < bytes || static_cast<std::size_t>(binding.offset) + bytes > buffer.Bytes()) {
    throw std::invalid_argument(
        std::string(operation) + ": parameter slot at " + std::to_string(binding.offset) +
        " with " + std::to_string(binding.size) + " bytes cannot hold " + std::to_string(bytes) +
        " bytes of a buffer of " + std::to_string(buffer.Bytes()) + " bytes");
  }
  return buffer.DeviceAddress() + binding.offset;
}

}  // namespace alcedo

#endif  // HAVE_VULKAN
