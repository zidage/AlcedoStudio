//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The kernel group `diffusion_filter` (src/edit/runtime/vulkan/shader/diffusion_filter.slang): the
// DiffusionFilter kernels of the OpenCL program opencl_dag_drt (drt.cl) that the DiffusionFilter
// pass dispatches (phase VK12).

#pragma once

#include <volk.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "vulkan/vulkan_compute_pipeline_cache.hpp"

namespace alcedo::vulkan::diffusion_filter_kernels {

/// ACEScc to linear AP1 (strength 0).
inline constexpr std::string_view kDecodeScene        = "diffusion_filter_decode_scene_rgba32f";
/// The highlight-boosted box reduction of the scene into the base level.
inline constexpr std::string_view kReduceBoost        = "diffusion_filter_reduce_boost";
/// One level of the 13-tap downsample.
inline constexpr std::string_view kDownsample         = "diffusion_filter_downsample";
/// The tent upsample of the coarser level plus the weighted level.
inline constexpr std::string_view kUpsampleAccumulate = "diffusion_filter_upsample_accumulate";
/// The decode of the scene and the mix of the scatter image.
inline constexpr std::string_view kMixScene           = "diffusion_filter_mix_scene_rgba32f";

/// Workgroup 16 x 16 x 1 of every kernel (the OpenCL local size).
inline constexpr uint32_t         kGroupSize          = 16;
/// The RGBA32F storage image of a cached scene; a dummy when the scene is a buffer.
inline constexpr uint32_t         kSceneBinding       = 0;
/// The RGBA32F storage image that a kernel writes (a level, or the scene output image).
inline constexpr uint32_t         kOutputBinding      = 1;
/// The RGBA32F storage image that a kernel samples: the finer level of the downsample, the
/// coarser level of the upsample, the scatter image of the mix.
inline constexpr uint32_t         kCoarseBinding      = 2;
/// The RGBA32F storage image of the own level of the upsample.
inline constexpr uint32_t         kLevelBinding       = 3;

/// Push constants of `diffusion_filter_decode_scene_rgba32f` (scalar layout).
struct DecodePushConstants {
  VkDeviceAddress source      = 0;  ///< RGBA32F row-major, or 0: the scene image binding
  VkDeviceAddress destination = 0;  ///< RGBA32F row-major, or 0: the output image binding
  uint32_t        width       = 0;
  uint32_t        height      = 0;
};
static_assert(sizeof(DecodePushConstants) == 24);

/// Push constants of `diffusion_filter_reduce_boost` (scalar layout).
struct ReducePushConstants {
  VkDeviceAddress source            = 0;  ///< RGBA32F row-major, or 0: the scene image binding
  int32_t         src_width         = 0;
  int32_t         src_height        = 0;
  int32_t         dst_width         = 0;
  int32_t         dst_height        = 0;
  int32_t         samples           = 1;
  float           gain              = 0.0F;
  float           low               = 0.0F;
  float           high              = 0.0F;
  float           base_to_render[6] = {};  ///< The first two rows of the 3 x 3 matrix
};
static_assert(sizeof(ReducePushConstants) == 64);

/// Push constants of `diffusion_filter_downsample` (scalar layout).
struct DownsamplePushConstants {
  int32_t src_width  = 0;
  int32_t src_height = 0;
  int32_t dst_width  = 0;
  int32_t dst_height = 0;
};
static_assert(sizeof(DownsamplePushConstants) == 16);

/// Push constants of `diffusion_filter_upsample_accumulate` (scalar layout).
struct UpsamplePushConstants {
  int32_t coarse_width  = 0;
  int32_t coarse_height = 0;
  int32_t width         = 0;
  int32_t height        = 0;
  float   coarse_weight = 0.0F;
  float   level_weight  = 0.0F;
};
static_assert(sizeof(UpsamplePushConstants) == 24);

/// Push constants of `diffusion_filter_mix_scene_rgba32f` (scalar layout).
struct MixPushConstants {
  VkDeviceAddress source            = 0;  ///< RGBA32F row-major, or 0: the scene image binding
  VkDeviceAddress destination       = 0;  ///< RGBA32F row-major, or 0: the output image binding
  int32_t         width             = 0;
  int32_t         height            = 0;
  int32_t         scatter_width     = 0;
  int32_t         scatter_height    = 0;
  float           scatter_fraction  = 0.0F;
  float           transmission      = 1.0F;
  float           render_to_base[6] = {};  ///< The first two rows of the 3 x 3 matrix
};
static_assert(sizeof(MixPushConstants) == 64);

template <class PushConstants>
[[nodiscard]] inline auto Descriptor(std::string_view                 kernel,
                                     std::vector<VulkanKernelBinding> bindings)
    -> VulkanKernelDescriptor {
  return {std::string(kernel), std::move(bindings), sizeof(PushConstants), {}};
}

[[nodiscard]] inline auto DecodeSceneDescriptor() -> VulkanKernelDescriptor {
  return Descriptor<DecodePushConstants>(kDecodeScene,
                                         {{kSceneBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                          {kOutputBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}});
}

[[nodiscard]] inline auto ReduceBoostDescriptor() -> VulkanKernelDescriptor {
  return Descriptor<ReducePushConstants>(kReduceBoost,
                                         {{kSceneBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                          {kOutputBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}});
}

[[nodiscard]] inline auto DownsampleDescriptor() -> VulkanKernelDescriptor {
  return Descriptor<DownsamplePushConstants>(kDownsample,
                                             {{kOutputBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                              {kCoarseBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}});
}

[[nodiscard]] inline auto UpsampleAccumulateDescriptor() -> VulkanKernelDescriptor {
  return Descriptor<UpsamplePushConstants>(kUpsampleAccumulate,
                                           {{kOutputBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                            {kCoarseBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                            {kLevelBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}});
}

[[nodiscard]] inline auto MixSceneDescriptor() -> VulkanKernelDescriptor {
  return Descriptor<MixPushConstants>(kMixScene,
                                      {{kSceneBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                       {kOutputBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                       {kCoarseBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}});
}

/// The descriptors of the five kernels of the group, for the kernel catalog.
[[nodiscard]] inline auto AllDescriptors() -> std::array<VulkanKernelDescriptor, 5> {
  return {DecodeSceneDescriptor(), ReduceBoostDescriptor(), DownsampleDescriptor(),
          UpsampleAccumulateDescriptor(), MixSceneDescriptor()};
}

}  // namespace alcedo::vulkan::diffusion_filter_kernels
