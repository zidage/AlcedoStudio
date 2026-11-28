//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The kernel group `mask` (src/edit/runtime/vulkan/shader/mask.slang): the kernels of the OpenCL
// program opencl_dag_mask (mask.cl) for MaskEvaluate, MaskUnion, and the coverage displays of a
// Color Grade Mask (phase VK11).

#pragma once

#include <volk.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "vulkan/vulkan_compute_pipeline_cache.hpp"

namespace alcedo::vulkan::mask_kernels {

/// The analytic MaskEvaluate (Radial, Linear Gradient) with the optional ranges, into R8.
inline constexpr std::string_view kAnalytic          = "mask_analytic_r8";
/// The raster MaskEvaluate (AI Mask) with the optional ranges, into R8.
inline constexpr std::string_view kRaster            = "mask_raster_r8";
/// The raster MaskEvaluate into an RGBA32F coverage display: (value, 0, 0, 1).
inline constexpr std::string_view kRasterDisplay     = "mask_raster_rgba32f";
/// Zero of each texel of an R8 Union output.
inline constexpr std::string_view kFillZero          = "mask_fill_zero_r8";
/// The Union maximum in place: output = max(output, source).
inline constexpr std::string_view kUnionMax          = "mask_union_max_r8";
/// The R8 value of a probed Mask into an RGBA32F display: (value, 0, 0, 1).
inline constexpr std::string_view kValueDisplay      = "mask_value_display_rgba32f";

/// Workgroup 16 x 16 x 1 of every kernel (the OpenCL local size).
inline constexpr uint32_t         kGroupSize         = 16;
/// The RGBA32F storage image of a cached Grade input (a ranged Mask); a dummy otherwise.
inline constexpr uint32_t         kSceneBinding      = 0;
/// The R8 storage image that a kernel reads: the coverage texture of the raster sampling, the
/// next source of the Union, the probed Mask of the value display.
inline constexpr uint32_t         kSourceBinding     = 1;
/// The R8 storage image that a kernel writes (the Union reads it too).
inline constexpr uint32_t         kOutputR8Binding   = 2;
/// The RGBA32F storage image of a coverage display.
inline constexpr uint32_t         kOutputRgbaBinding = 3;

/// The `shape` values of a Radial Mask in @ref AnalyticPushConstants.
enum class RadialShape : uint32_t {
  kCenterX = 0,
  kCenterY,
  kMajorRadius,
  kMinorRadius,
  kRotation,
  kInnerFeather,
  kOuterFeather
};

/// The `shape` values of a Linear Gradient in @ref AnalyticPushConstants.
enum class LinearShape : uint32_t {
  kOriginX = 0,
  kOriginY,
  kNormalX,
  kNormalY,
  kTransitionDistance,
  kStartValue,
  kEndValue
};

/// Push constants of `mask_analytic_r8` (scalar layout). The fields of `MaskAnalyticParams` of
/// mask.cl; the Radial and the Linear Gradient values share `shape` (kind 0: Radial, 1: Linear
/// Gradient), and a ranged Mask adds the Grade input and its ranges.
struct AnalyticPushConstants {
  VkDeviceAddress scene                  = 0;   ///< RGBA32F row-major, or 0: the scene image
  float           render_to_reference[6] = {};  ///< The first two rows of the 3 x 3 matrix
  uint32_t        width                  = 0;
  uint32_t        height                 = 0;
  uint32_t        reference_width        = 0;
  uint32_t        reference_height       = 0;
  uint32_t        kind                   = 0;
  uint32_t        invert                 = 0;
  float           shape[7]               = {};
  float           opacity                = 1.0F;
  /// The bytes of `MaskRangeParams` (mask_model.hpp, nine 4-byte fields); both `*_restricted`
  /// flags zero: no read of the Grade input.
  uint32_t        range[9]               = {};
  uint32_t        pad                    = 0;
};
static_assert(sizeof(AnalyticPushConstants) == 128);

/// Push constants of `mask_raster_r8` and `mask_raster_rgba32f` (scalar layout): the fields of
/// `MaskRasterParams` of mask.cl, the Grade input, and the ranges.
struct RasterPushConstants {
  VkDeviceAddress scene                   = 0;   ///< RGBA32F row-major, or 0: the scene image
  float           render_to_texture_uv[6] = {};  ///< The first two rows of the 3 x 3 matrix
  uint32_t        width                   = 0;
  uint32_t        height                  = 0;
  uint32_t        texture_width           = 0;
  uint32_t        texture_height          = 0;
  uint32_t        samples                 = 1;
  uint32_t        invert                  = 0;
  float           opacity                 = 1.0F;
  /// The bytes of `MaskRangeParams`, as in @ref AnalyticPushConstants.
  uint32_t        range[9]                = {};
};
static_assert(sizeof(RasterPushConstants) == 96);

/// Push constants of the zero fill, the Union, and the value display (scalar layout).
struct ExtentPushConstants {
  uint32_t width  = 0;
  uint32_t height = 0;
};
static_assert(sizeof(ExtentPushConstants) == 8);

template <class PushConstants>
[[nodiscard]] inline auto Descriptor(std::string_view                 kernel,
                                     std::vector<VulkanKernelBinding> bindings)
    -> VulkanKernelDescriptor {
  return {std::string(kernel), std::move(bindings), sizeof(PushConstants), {}};
}

[[nodiscard]] inline auto AnalyticDescriptor() -> VulkanKernelDescriptor {
  return Descriptor<AnalyticPushConstants>(kAnalytic,
                                           {{kSceneBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                            {kOutputR8Binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}});
}

[[nodiscard]] inline auto RasterDescriptor() -> VulkanKernelDescriptor {
  return Descriptor<RasterPushConstants>(kRaster,
                                         {{kSceneBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                          {kSourceBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                          {kOutputR8Binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}});
}

[[nodiscard]] inline auto RasterDisplayDescriptor() -> VulkanKernelDescriptor {
  return Descriptor<RasterPushConstants>(kRasterDisplay,
                                         {{kSceneBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                          {kSourceBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                          {kOutputRgbaBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}});
}

[[nodiscard]] inline auto FillZeroDescriptor() -> VulkanKernelDescriptor {
  return Descriptor<ExtentPushConstants>(kFillZero,
                                         {{kOutputR8Binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}});
}

[[nodiscard]] inline auto UnionMaxDescriptor() -> VulkanKernelDescriptor {
  return Descriptor<ExtentPushConstants>(kUnionMax,
                                         {{kSourceBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                          {kOutputR8Binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}});
}

[[nodiscard]] inline auto ValueDisplayDescriptor() -> VulkanKernelDescriptor {
  return Descriptor<ExtentPushConstants>(kValueDisplay,
                                         {{kSourceBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                          {kOutputRgbaBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}});
}

/// The descriptors of the six kernels of the group, for the kernel catalog.
[[nodiscard]] inline auto AllDescriptors() -> std::array<VulkanKernelDescriptor, 6> {
  return {AnalyticDescriptor(), RasterDescriptor(),   RasterDisplayDescriptor(),
          FillZeroDescriptor(), UnionMaxDescriptor(), ValueDisplayDescriptor()};
}

}  // namespace alcedo::vulkan::mask_kernels
