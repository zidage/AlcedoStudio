//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "vulkan/vulkan_kernel_catalog.hpp"

#include "vulkan/vulkan_color_math_check_kernels.hpp"
#include "vulkan/vulkan_debayer_rcd_kernels.hpp"
#include "vulkan/vulkan_display_to_ap1_kernels.hpp"
#include "vulkan/vulkan_dng_warp_kernels.hpp"
#include "vulkan/vulkan_geometry_camera_kernels.hpp"
#include "vulkan/vulkan_geometry_kernels.hpp"
#include "vulkan/vulkan_highlight_kernels.hpp"
#include "vulkan/vulkan_lens_calib_kernels.hpp"
#include "vulkan/vulkan_primary_grade_kernels.hpp"
#include "vulkan/vulkan_primary_grade_neighbor_kernels.hpp"
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
      highlight_kernels::ReconstructPlanarPackDescriptor(),
      dng_warp_kernels::WarpRectilinearDescriptor(),
      lens_calib_kernels::VignettingDescriptor(),
      lens_calib_kernels::WarpDescriptor(),
      geometry_camera_kernels::GeometryResampleDescriptor(),
      geometry_camera_kernels::CameraColorDescriptor(),
      display_to_ap1_kernels::LinearizeRasterDescriptor(),
      display_to_ap1_kernels::DisplayToAp1Descriptor(display_to_ap1_kernels::kDisplayToAp1),
      display_to_ap1_kernels::DisplayToAp1Descriptor(display_to_ap1_kernels::kDisplayToAp0Linear),
      primary_grade_kernels::PointwiseSceneDescriptor(),
      primary_grade_kernels::CurveInputHistogramDescriptor(),
      primary_grade_neighbor_kernels::BlurDescriptor(),
      primary_grade_neighbor_kernels::ApplyDescriptor(),
  };
  for (const auto kernel : debayer_rcd_kernels::kRcdKernels) {
    catalog.push_back(debayer_rcd_kernels::Descriptor(kernel));
  }
  for (const auto kernel : highlight_kernels::kRgbaKernels) {
    catalog.push_back(highlight_kernels::RgbaDescriptor(kernel));
  }
  for (const auto kernel : highlight_kernels::kPlanarKernels) {
    catalog.push_back(highlight_kernels::PlanarDescriptor(kernel));
  }
  return catalog;
}

}  // namespace alcedo::vulkan
