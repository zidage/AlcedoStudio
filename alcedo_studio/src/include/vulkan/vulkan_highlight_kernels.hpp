//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The kernel group `highlight_reconstruct`
// (src/decoders/processor/operators/gpu/vulkan_shader/highlight_reconstruct.slang): the opposed
// highlight reconstruction of the OpenCL program raw_processor_highlight (phase VK5).

#pragma once

#include <volk.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "vulkan/vulkan_compute_pipeline_cache.hpp"
#include "vulkan/vulkan_raw_pack_kernels.hpp"

namespace alcedo::vulkan::highlight_kernels {

/// RGBA source: the clip mask.
inline constexpr std::string_view kBuildMask                = "hlr_build_mask";
/// Both forms: the 7 x 7 dilation of the clip mask.
inline constexpr std::string_view kDilateMask               = "hlr_dilate_mask";
/// RGBA source: the workgroup sums of the chrominance ring into the statistics.
inline constexpr std::string_view kChrominanceContrib       = "hlr_chrominance_contrib";
/// RGBA source: the reconstruction into the RGBA destination buffer.
inline constexpr std::string_view kReconstructFromStats     = "hlr_reconstruct_from_stats";
/// Planar source with a crop: the clip mask.
inline constexpr std::string_view kBuildMaskPlanar          = "hlr_build_mask_planar";
/// Planar source with a crop: the chrominance sums.
inline constexpr std::string_view kChrominanceContribPlanar = "hlr_chrominance_contrib_planar";
/// Planar source with a crop: the reconstruction and the pack into the destination image.
inline constexpr std::string_view kReconstructPlanarPack = "hlr_reconstruct_from_stats_planar_pack";
/// Workgroup 16 x 16 x 1 for every kernel; the chrominance kernels reduce 256 invocations.
inline constexpr uint32_t         kGroupSize             = 16;
/// The RGBA32F storage image of `hlr_reconstruct_from_stats_planar_pack` (set 0).
inline constexpr uint32_t         kDestinationBinding    = 0;

/// The bit of each channel in a mask word (one 32-bit word for each texel).
inline constexpr uint32_t         kMaskR                 = 1;
inline constexpr uint32_t         kMaskG                 = 2;
inline constexpr uint32_t         kMaskB                 = 4;

/// The statistics record: the float sums of R, G, B, then the float counts of R, G, B.
inline constexpr uint32_t         kStatisticsWords       = 6;

/// `HighlightLimits` of the Slang source (scalar layout): the OpenCL `HighlightParams` without
/// the chrominance, with named fields.
struct HighlightLimits {
  float    clip_r     = 0.0F;
  float    clip_g     = 0.0F;
  float    clip_b     = 0.0F;
  float    clipdark_r = 0.0F;
  float    clipdark_g = 0.0F;
  float    clipdark_b = 0.0F;
  uint32_t width      = 0;  ///< texels of the reconstructed region in a row
  uint32_t height     = 0;  ///< rows of the reconstructed region
  uint32_t stride     = 0;  ///< texels in a row of the source
};
static_assert(sizeof(HighlightLimits) == 36);

/// Push constants of the RGBA kernels and of `hlr_dilate_mask` (scalar layout).
struct HighlightRgbaPushConstants {
  VkDeviceAddress source      = 0;  ///< RGBA float values
  VkDeviceAddress destination = 0;  ///< RGBA float values
  VkDeviceAddress mask        = 0;  ///< width * height words
  VkDeviceAddress dilated     = 0;  ///< width * height words
  VkDeviceAddress statistics  = 0;  ///< kStatisticsWords words
  HighlightLimits limits;
  uint32_t        reserved = 0;
};
static_assert(sizeof(HighlightRgbaPushConstants) == 80);

/// Push constants of `hlr_build_mask_planar` and `hlr_chrominance_contrib_planar`.
struct HighlightPlanarPushConstants {
  VkDeviceAddress r          = 0;
  VkDeviceAddress g          = 0;
  VkDeviceAddress b          = 0;
  VkDeviceAddress mask       = 0;
  VkDeviceAddress dilated    = 0;
  VkDeviceAddress statistics = 0;
  HighlightLimits limits;
  uint32_t        crop_x   = 0;
  uint32_t        crop_y   = 0;
  uint32_t        reserved = 0;
};
static_assert(sizeof(HighlightPlanarPushConstants) == 96);

/// Push constants of `hlr_reconstruct_from_stats_planar_pack`.
struct HighlightPlanarPackPushConstants {
  VkDeviceAddress                    r          = 0;
  VkDeviceAddress                    g          = 0;
  VkDeviceAddress                    b          = 0;
  VkDeviceAddress                    statistics = 0;
  HighlightLimits                    limits;
  raw_pack_kernels::PackOrientParams pack;
};
static_assert(sizeof(HighlightPlanarPackPushConstants) == 120);

/// The descriptor of an RGBA kernel or of `hlr_dilate_mask`: @p name is kBuildMask, kDilateMask,
/// kChrominanceContrib, or kReconstructFromStats.
[[nodiscard]] inline auto RgbaDescriptor(std::string_view name) -> VulkanKernelDescriptor {
  return {std::string(name), {}, sizeof(HighlightRgbaPushConstants), {}};
}

/// The descriptor of kBuildMaskPlanar or kChrominanceContribPlanar.
[[nodiscard]] inline auto PlanarDescriptor(std::string_view name) -> VulkanKernelDescriptor {
  return {std::string(name), {}, sizeof(HighlightPlanarPushConstants), {}};
}

/// The RGBA kernels and `hlr_dilate_mask`, in the order of the RGBA encode function.
inline constexpr std::array<std::string_view, 4> kRgbaKernels = {
    kBuildMask, kDilateMask, kChrominanceContrib, kReconstructFromStats};
/// The planar kernels without an image.
inline constexpr std::array<std::string_view, 2> kPlanarKernels = {kBuildMaskPlanar,
                                                                   kChrominanceContribPlanar};

[[nodiscard]] inline auto ReconstructPlanarPackDescriptor() -> VulkanKernelDescriptor {
  return {std::string(kReconstructPlanarPack),
          {{kDestinationBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}},
          sizeof(HighlightPlanarPackPushConstants),
          {}};
}

}  // namespace alcedo::vulkan::highlight_kernels
