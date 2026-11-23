//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <volk.h>

#include <stdexcept>
#include <string>
#include <string_view>

namespace alcedo::vulkan {

/// The enumerator name of @p result, for example `VK_ERROR_DEVICE_LOST`.
[[nodiscard]] auto VulkanResultName(VkResult result) -> std::string_view;

/**
 * @brief The error of a failed Vulkan call. `what()` is `<operation> failed: <VkResult name>`.
 *
 * The plan (section 2.4) reports each Vulkan failure with its `VkResult`; this type keeps the
 * value for callers that act on it, for example on `VK_ERROR_DEVICE_LOST`.
 */
class VulkanError : public std::runtime_error {
 public:
  VulkanError(std::string_view operation, VkResult result);

  [[nodiscard]] auto Result() const noexcept -> VkResult { return result_; }

 private:
  VkResult result_;
};

/// Throws `VulkanError` when @p result is negative (an error code). A positive success code such
/// as `VK_INCOMPLETE` does not throw.
void ThrowIfVulkanError(VkResult result, std::string_view operation);

}  // namespace alcedo::vulkan
