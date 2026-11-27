//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The kernel group `geometry_camera` (src/edit/runtime/vulkan/shader/geometry_camera.slang): the
// geometry resample and the camera color kernels of the OpenCL program opencl_dag_geometry_camera
// (phase VK7).

#pragma once

#include <volk.h>

#include <cstdint>
#include <string>
#include <string_view>

#include "vulkan/vulkan_compute_pipeline_cache.hpp"

namespace alcedo::vulkan::geometry_camera_kernels {

/// The bilinear or bicubic resample of the decoded image into the render extent.
inline constexpr std::string_view kGeometryResample   = "geometry_resample_rgba32f";
/// Camera RGB to ACEScc AP1: matrix, DNG profile tables, reference gamut compression.
inline constexpr std::string_view kCameraColor        = "camera_color_acescc";
/// Workgroup 16 x 16 x 1 for the two kernels.
inline constexpr uint32_t         kGroupSize          = 16;
/// The RGBA32F storage image that a kernel reads (set 0).
inline constexpr uint32_t         kSourceBinding      = 0;
/// The RGBA32F storage image that a kernel writes (set 0).
inline constexpr uint32_t         kDestinationBinding = 1;

/// Push constants of `geometry_resample_rgba32f` (scalar layout): the OpenCL
/// `GeometryResampleParams`.
struct GeometryResamplePushConstants {
  float    m00            = 1.0F;  ///< render_to_decoded, row-major 2 x 3
  float    m01            = 0.0F;
  float    m02            = 0.0F;
  float    m10            = 0.0F;
  float    m11            = 1.0F;
  float    m12            = 0.0F;
  uint32_t decoded_width  = 0;
  uint32_t decoded_height = 0;
  uint32_t render_width   = 0;
  uint32_t render_height  = 0;
  float    border[4]      = {};
  uint32_t use_bicubic    = 0;
};
static_assert(sizeof(GeometryResamplePushConstants) == 60);

/// Push constants of `camera_color_acescc` (scalar layout).
struct CameraColorPushConstants {
  VkDeviceAddress camera_to_ap1 = 0;  ///< 9 floats, row-major (`CameraColorGpuParams`)
  VkDeviceAddress dng_profile   = 0;  ///< the `PackDngProfileGpuData` tables
  uint32_t        width         = 0;
  uint32_t        height        = 0;
};
static_assert(sizeof(CameraColorPushConstants) == 24);

[[nodiscard]] inline auto GeometryResampleDescriptor() -> VulkanKernelDescriptor {
  return {std::string(kGeometryResample),
          {{kSourceBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
           {kDestinationBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}},
          sizeof(GeometryResamplePushConstants),
          {}};
}

[[nodiscard]] inline auto CameraColorDescriptor() -> VulkanKernelDescriptor {
  return {std::string(kCameraColor),
          {{kSourceBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
           {kDestinationBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}},
          sizeof(CameraColorPushConstants),
          {}};
}

}  // namespace alcedo::vulkan::geometry_camera_kernels
