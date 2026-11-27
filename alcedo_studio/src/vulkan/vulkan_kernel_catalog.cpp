//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "vulkan/vulkan_kernel_catalog.hpp"

#include "vulkan/vulkan_color_math_check_kernels.hpp"
#include "vulkan/vulkan_geometry_kernels.hpp"
#include "vulkan/vulkan_test_fill_kernels.hpp"

namespace alcedo::vulkan {

auto VulkanKernelCatalog() -> std::vector<VulkanKernelDescriptor> {
  return {
      test_fill_kernels::FillBufferDescriptor(),
      test_fill_kernels::FillImageDescriptor(),
      color_math_check_kernels::TransferDescriptor(),
      color_math_check_kernels::ColorDescriptor(),
      geometry_kernels::CropResizeLinearDescriptor(),
  };
}

}  // namespace alcedo::vulkan
