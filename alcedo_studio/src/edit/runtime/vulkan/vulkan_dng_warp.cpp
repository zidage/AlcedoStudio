//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_VULKAN

#include "edit/runtime/vulkan/vulkan_dng_warp.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "edit/runtime/texture_format.hpp"
#include "edit/runtime/vulkan/vulkan_kernel_dispatch.hpp"
#include "vulkan/vulkan_dng_warp_kernels.hpp"

namespace alcedo::vulkan::geometry {
namespace {

[[noreturn]] void ThrowWarpError(const std::string& message) {
  throw std::invalid_argument("vulkan::geometry::EncodeWarpRectilinear: " + message);
}

void RequireRgba32f(const VulkanBackend::Texture2D& texture, const char* role) {
  if (texture.Empty()) {
    ThrowWarpError(std::string("empty ") + role + " texture");
  }
  if (texture.Format() != TextureFormat::Rgba32f) {
    ThrowWarpError(std::string(role) + " texture is " + TextureFormatName(texture.Format()) +
                   "; RGBA32F is required");
  }
}

void CopySet(const std::array<double, 6>& set, float (&out)[6]) {
  for (std::size_t i = 0; i < set.size(); ++i) {
    out[i] = static_cast<float>(set[i]);
  }
}

}  // namespace

void EncodeWarpRectilinear(VulkanBackend& backend, VulkanCommandContext& command_context,
                           const VulkanBackend::Texture2D& source,
                           VulkanBackend::Texture2D&       destination,
                           const dng::WarpRectilinear&     warp) {
  RequireRgba32f(source, "source");
  RequireRgba32f(destination, "destination");
  if (source.ResourceId() == destination.ResourceId()) {
    ThrowWarpError("the source and the destination are one texture");
  }
  if (source.Width() != destination.Width() || source.Height() != destination.Height()) {
    ThrowWarpError("the source is " + std::to_string(source.Width()) + "x" +
                   std::to_string(source.Height()) + " and the destination is " +
                   std::to_string(destination.Width()) + "x" +
                   std::to_string(destination.Height()) + "; the sizes must be equal");
  }
  if (warp.coefficient_set_count == 0 || warp.coefficient_set_count > 3) {
    ThrowWarpError("the opcode has " + std::to_string(warp.coefficient_set_count) +
                   " coefficient sets; 1 to 3 are valid");
  }

  // The OpenCL EncodeWarp: the sets past the count stay as parsed (zero); the kernel reads only
  // the first set when the count is 1.
  dng_warp_kernels::WarpRectilinearPushConstants push;
  push.coefficient_set_count = warp.coefficient_set_count;
  push.width                 = destination.Width();
  push.height                = destination.Height();
  push.center_x              = static_cast<float>(warp.center_x);
  push.center_y              = static_cast<float>(warp.center_y);
  CopySet(warp.coefficient_sets[0], push.set_r);
  CopySet(warp.coefficient_sets[1], push.set_g);
  CopySet(warp.coefficient_sets[2], push.set_b);

  const std::array<VulkanImageBinding, 2> images = {
      VulkanImageBinding{dng_warp_kernels::kSourceBinding, &source},
      VulkanImageBinding{dng_warp_kernels::kDestinationBinding, &destination},
  };
  RecordKernelDispatch(
      backend, command_context, dng_warp_kernels::WarpRectilinearDescriptor(), images,
      PushConstantBytes(push),
      WorkgroupsCovering(destination.Width(), destination.Height(), dng_warp_kernels::kGroupSize,
                         dng_warp_kernels::kGroupSize));
}

}  // namespace alcedo::vulkan::geometry

#endif  // HAVE_VULKAN
