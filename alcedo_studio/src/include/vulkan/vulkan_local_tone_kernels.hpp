//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The kernel group `local_tone` (src/edit/runtime/vulkan/shader/local_tone.slang): the kernels of
// the OpenCL program opencl_dag_local_tone (local_tone.cl) that the Local Laplacian stage of a
// Color Grade dispatches (Shadows, Highlights; phase VK10), and three kernels on device addresses
// for the zero fill of a plane and the copies between a canonical R32F image and a plane.

#pragma once

#include <volk.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "vulkan/vulkan_compute_pipeline_cache.hpp"

namespace alcedo::vulkan::local_tone_kernels {

/// The log intensity of a scene image, resized to the plane (`local_tone_extract_scene`).
inline constexpr std::string_view kExtract          = "local_tone_extract_scene";
/// The log intensity of the canonical plane through the reference-to-render transform.
inline constexpr std::string_view kExtractReference = "local_tone_extract_reference_scene";
/// One level down the Gaussian pyramid of a plane.
inline constexpr std::string_view kPyramidDown      = "local_tone_pyr_down";
/// The remap of the source plane around one sample.
inline constexpr std::string_view kRemap            = "local_tone_remap";
/// The Laplacian of two samples, interpolated by the source value.
inline constexpr std::string_view kSelect           = "local_tone_select";
/// One level up the Laplacian pyramid.
inline constexpr std::string_view kCollapse         = "local_tone_collapse";
/// The tone of the working scene-work member and the optional (masked) mix.
inline constexpr std::string_view kApply            = "local_tone_apply_scene";
/// Zero of each texel of a plane.
inline constexpr std::string_view kFillZero         = "local_tone_fill_zero";
/// A canonical R32F image into a plane.
inline constexpr std::string_view kCopyImageToPlane = "local_tone_copy_image_to_plane";
/// A plane into a canonical R32F image.
inline constexpr std::string_view kCopyPlaneToImage = "local_tone_copy_plane_to_image";

/// Workgroup 16 x 16 x 1 of every kernel (the OpenCL local size).
inline constexpr uint32_t         kGroupSize        = 16;
/// The RGBA32F storage image of a cached scene source (the extract input, the mix source of the
/// apply); a dummy when the scene is a scene-work buffer.
inline constexpr uint32_t         kSceneBinding     = 0;
/// The R8 storage image of the mask of the apply; a dummy without a mask.
inline constexpr uint32_t         kMaskBinding      = 1;
/// The R32F storage image of a canonical plane (the two copies).
inline constexpr uint32_t         kPlaneBinding     = 2;

/// Push constants of `local_tone_extract_scene` (scalar layout).
struct ExtractPushConstants {
  VkDeviceAddress source        = 0;  ///< RGBA32F row-major, or 0: the scene image binding
  VkDeviceAddress output        = 0;  ///< R32F plane
  int32_t         input_width   = 0;
  int32_t         input_height  = 0;
  int32_t         output_width  = 0;
  int32_t         output_height = 0;
};
static_assert(sizeof(ExtractPushConstants) == 32);

/// Push constants of `local_tone_extract_reference_scene` (scalar layout).
struct ExtractReferencePushConstants {
  VkDeviceAddress source                 = 0;  ///< RGBA32F row-major, or 0: the scene image
  VkDeviceAddress output                 = 0;  ///< R32F plane
  int32_t         input_width            = 0;
  int32_t         input_height           = 0;
  int32_t         output_width           = 0;
  int32_t         output_height          = 0;
  float           full_ref_w             = 0.0F;
  float           full_ref_h             = 0.0F;
  float           reference_to_render[6] = {};  ///< The first two rows of the 3 x 3 matrix
};
static_assert(sizeof(ExtractReferencePushConstants) == 64);

/// Push constants of `local_tone_pyr_down` (scalar layout).
struct PyramidDownPushConstants {
  VkDeviceAddress source     = 0;
  VkDeviceAddress output     = 0;
  int32_t         src_width  = 0;
  int32_t         src_height = 0;
  int32_t         dst_width  = 0;
  int32_t         dst_height = 0;
};
static_assert(sizeof(PyramidDownPushConstants) == 32);

/// Push constants of `local_tone_remap` (scalar layout).
struct RemapPushConstants {
  VkDeviceAddress source = 0;
  VkDeviceAddress output = 0;
  int32_t         width  = 0;
  int32_t         height = 0;
  float           gamma  = 0.0F;
  float           target = 0.0F;
  float           beta   = 1.0F;
  float           alpha  = 1.0F;
  float           sigma  = 0.0F;
  int32_t         pad    = 0;
};
static_assert(sizeof(RemapPushConstants) == 48);

/// Push constants of `local_tone_select` (scalar layout).
struct SelectPushConstants {
  VkDeviceAddress source        = 0;
  VkDeviceAddress lo            = 0;
  VkDeviceAddress lo_coarse     = 0;
  VkDeviceAddress hi            = 0;
  VkDeviceAddress hi_coarse     = 0;
  VkDeviceAddress output        = 0;
  int32_t         width         = 0;
  int32_t         height        = 0;
  int32_t         coarse_width  = 0;
  int32_t         coarse_height = 0;
  float           gamma_lo      = 0.0F;
  float           gamma_hi      = 0.0F;
  int32_t         first         = 0;
  int32_t         last          = 0;
  int32_t         top           = 0;
  int32_t         pad           = 0;
};
static_assert(sizeof(SelectPushConstants) == 88);

/// Push constants of `local_tone_collapse` (scalar layout).
struct CollapsePushConstants {
  VkDeviceAddress laplacian     = 0;
  VkDeviceAddress coarse        = 0;
  VkDeviceAddress output        = 0;
  int32_t         width         = 0;
  int32_t         height        = 0;
  int32_t         coarse_width  = 0;
  int32_t         coarse_height = 0;
};
static_assert(sizeof(CollapsePushConstants) == 40);

/// Push constants of `local_tone_apply_scene` (scalar layout).
struct ApplyPushConstants {
  VkDeviceAddress working         = 0;  ///< RGBA32F row-major (a scene-work member)
  VkDeviceAddress mix_source      = 0;  ///< RGBA32F row-major, or 0: the scene image binding
  VkDeviceAddress reference       = 0;  ///< R32F plane
  VkDeviceAddress adjusted        = 0;  ///< R32F plane
  int32_t         width           = 0;
  int32_t         height          = 0;
  int32_t         adjusted_width  = 0;
  int32_t         adjusted_height = 0;
  float           render_to_uv[6] = {};  ///< The first two rows of the 3 x 3 matrix
  int32_t         apply_mix       = 0;
  int32_t         has_mask        = 0;
  float           grade_mix       = 1.0F;
  int32_t         pad             = 0;
};
static_assert(sizeof(ApplyPushConstants) == 88);

/// Push constants of `local_tone_fill_zero` (scalar layout): the texels of the plane in rows of
/// `row_texels`, so a large plane stays inside the workgroup count limit.
struct FillPushConstants {
  VkDeviceAddress plane      = 0;  ///< R32F plane of `count` texels
  uint32_t        count      = 0;
  uint32_t        row_texels = 0;
};
static_assert(sizeof(FillPushConstants) == 16);

/// Push constants of the two copies (scalar layout).
struct PlanePushConstants {
  VkDeviceAddress plane  = 0;  ///< R32F plane of width x height texels
  int32_t         width  = 0;
  int32_t         height = 0;
};
static_assert(sizeof(PlanePushConstants) == 16);

template <class PushConstants>
[[nodiscard]] inline auto Descriptor(std::string_view                 kernel,
                                     std::vector<VulkanKernelBinding> bindings)
    -> VulkanKernelDescriptor {
  return {std::string(kernel), std::move(bindings), sizeof(PushConstants), {}};
}

[[nodiscard]] inline auto ExtractDescriptor() -> VulkanKernelDescriptor {
  return Descriptor<ExtractPushConstants>(kExtract,
                                          {{kSceneBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}});
}

[[nodiscard]] inline auto ExtractReferenceDescriptor() -> VulkanKernelDescriptor {
  return Descriptor<ExtractReferencePushConstants>(
      kExtractReference, {{kSceneBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}});
}

[[nodiscard]] inline auto PyramidDownDescriptor() -> VulkanKernelDescriptor {
  return Descriptor<PyramidDownPushConstants>(kPyramidDown, {});
}

[[nodiscard]] inline auto RemapDescriptor() -> VulkanKernelDescriptor {
  return Descriptor<RemapPushConstants>(kRemap, {});
}

[[nodiscard]] inline auto SelectDescriptor() -> VulkanKernelDescriptor {
  return Descriptor<SelectPushConstants>(kSelect, {});
}

[[nodiscard]] inline auto CollapseDescriptor() -> VulkanKernelDescriptor {
  return Descriptor<CollapsePushConstants>(kCollapse, {});
}

[[nodiscard]] inline auto ApplyDescriptor() -> VulkanKernelDescriptor {
  return Descriptor<ApplyPushConstants>(kApply, {{kSceneBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                                 {kMaskBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}});
}

[[nodiscard]] inline auto FillZeroDescriptor() -> VulkanKernelDescriptor {
  return Descriptor<FillPushConstants>(kFillZero, {});
}

[[nodiscard]] inline auto CopyImageToPlaneDescriptor() -> VulkanKernelDescriptor {
  return Descriptor<PlanePushConstants>(kCopyImageToPlane,
                                        {{kPlaneBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}});
}

[[nodiscard]] inline auto CopyPlaneToImageDescriptor() -> VulkanKernelDescriptor {
  return Descriptor<PlanePushConstants>(kCopyPlaneToImage,
                                        {{kPlaneBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}});
}

/// The descriptors of the ten kernels of the group, for the kernel catalog.
[[nodiscard]] inline auto AllDescriptors() -> std::array<VulkanKernelDescriptor, 10> {
  return {ExtractDescriptor(),         ExtractReferenceDescriptor(), PyramidDownDescriptor(),
          RemapDescriptor(),           SelectDescriptor(),           CollapseDescriptor(),
          ApplyDescriptor(),           FillZeroDescriptor(),         CopyImageToPlaneDescriptor(),
          CopyPlaneToImageDescriptor()};
}

}  // namespace alcedo::vulkan::local_tone_kernels
