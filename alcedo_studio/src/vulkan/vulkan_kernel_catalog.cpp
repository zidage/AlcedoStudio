//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "vulkan/vulkan_kernel_catalog.hpp"

#include "vulkan/vulkan_color_math_check_kernels.hpp"
#include "vulkan/vulkan_debayer_rcd_kernels.hpp"
#include "vulkan/vulkan_geometry_kernels.hpp"
#include "vulkan/vulkan_raw_core_kernels.hpp"
#include "vulkan/vulkan_raw_pack_kernels.hpp"
#include "vulkan/vulkan_test_fill_kernels.hpp"
#include "vulkan/vulkan_xtrans_kernels.hpp"

namespace alcedo::vulkan {

auto VulkanKernelCatalog() -> std::vector<VulkanKernelDescriptor> {
  std::vector<VulkanKernelDescriptor> catalog = {
      test_fill_kernels::FillBufferDescriptor(),
      test_fill_kernels::FillImageDescriptor(),
      color_math_check_kernels::TransferDescriptor(),
      color_math_check_kernels::ColorDescriptor(),
      geometry_kernels::CropResizeLinearDescriptor(),
      raw_core_kernels::LinearizeRgbDescriptor(),
      raw_core_kernels::ToLinearRefDescriptor(),
      raw_core_kernels::CfaClamp01Descriptor(),
      xtrans_kernels::GreenDescriptor(),
      xtrans_kernels::RgbaDescriptor(),
      raw_pack_kernels::PackPlanesDescriptor(),
      raw_pack_kernels::CopyRgbaDescriptor(),
  };
  for (const auto kernel : debayer_rcd_kernels::kRcdKernels) {
    catalog.push_back(debayer_rcd_kernels::Descriptor(kernel));
  }
  return catalog;
}

}  // namespace alcedo::vulkan
