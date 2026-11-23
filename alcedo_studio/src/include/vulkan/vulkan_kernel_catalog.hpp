//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <vector>

#include "vulkan/vulkan_compute_pipeline_cache.hpp"

namespace alcedo::vulkan {

/**
 * @brief The descriptor of each kernel of the application, from the headers of the kernel groups.
 *
 * `alcedo-gpu-check --vulkan` creates the pipeline of each one and checks that the kernel
 * inventory and this list have the same kernels. Each new kernel group adds its descriptors here.
 * Each descriptor has the default specialization of its kernel.
 */
[[nodiscard]] auto VulkanKernelCatalog() -> std::vector<VulkanKernelDescriptor>;

}  // namespace alcedo::vulkan
