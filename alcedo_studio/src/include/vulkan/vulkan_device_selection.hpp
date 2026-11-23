//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <span>
#include <string_view>

#include "vulkan/vulkan_device_capabilities.hpp"

namespace alcedo::vulkan {

/**
 * @brief Selects the physical device of the pipeline (plan section 9.2, VX2 rule).
 *
 * @param devices The capabilities of each physical device, in the enumeration order of the
 *        instance.
 * @param name_filter Part of a device name, case-insensitive (`ALCEDO_VULKAN_DEVICE`). Empty
 *        means no filter.
 * @return The index in @p devices of the selected device: among the devices that the filter
 *         accepts and that pass `CheckVulkanDeviceRequirements`, the first discrete GPU, then the
 *         first integrated GPU, then the first other device. Qt Quick on Direct3D 11 also
 *         prefers the discrete GPU.
 * @throws std::runtime_error When no device remains. The message names the filter, or each
 *         rejected device with its absent requirements. Pure function.
 */
[[nodiscard]] auto SelectVulkanDevice(std::span<const VulkanDeviceCapabilities> devices,
                                      std::string_view name_filter) -> std::size_t;

}  // namespace alcedo::vulkan
