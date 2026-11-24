//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_VULKAN

#include "edit/runtime/vulkan/vulkan_geometry.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "edit/runtime/texture_format.hpp"
#include "edit/runtime/vulkan/vulkan_kernel_dispatch.hpp"
#include "vulkan/vulkan_geometry_kernels.hpp"

namespace alcedo::vulkan::geometry {
namespace {

[[noreturn]] void ThrowCropResizeError(const std::string& message) {
  throw std::invalid_argument("vulkan::geometry::EncodeCropResize: " + message);
}

void RequireRgba32f(const VulkanBackend::Texture2D& texture, const char* role) {
  if (texture.Empty()) {
    ThrowCropResizeError(std::string("empty ") + role + " texture");
  }
  if (texture.Format() != TextureFormat::Rgba32f) {
    ThrowCropResizeError(std::string(role) + " texture is " + TextureFormatName(texture.Format()) +
                         "; RGBA32F is required");
  }
}

}  // namespace

void EncodeCropResize(VulkanBackend& backend, VulkanCommandContext& command_context,
                      const VulkanBackend::Texture2D& source, VulkanBackend::Texture2D& destination,
                      RectI crop) {
  RequireRgba32f(source, "source");
  RequireRgba32f(destination, "destination");
  if (source.ResourceId() == destination.ResourceId()) {
    ThrowCropResizeError("the source and the destination are one texture");
  }
  const auto source_width  = static_cast<std::int32_t>(source.Width());
  const auto source_height = static_cast<std::int32_t>(source.Height());
  if (crop.width <= 0 || crop.height <= 0 || crop.x < 0 || crop.y < 0 || crop.X1() > source_width ||
      crop.Y1() > source_height) {
    ThrowCropResizeError("crop " + std::to_string(crop.x) + "," + std::to_string(crop.y) + " " +
                         std::to_string(crop.width) + "x" + std::to_string(crop.height) +
                         " is empty or not inside the source of " + std::to_string(source_width) +
                         "x" + std::to_string(source_height));
  }

  const auto destination_width  = static_cast<std::int32_t>(destination.Width());
  const auto destination_height = static_cast<std::int32_t>(destination.Height());
  const bool full_source =
      crop.x == 0 && crop.y == 0 && crop.width == source_width && crop.height == source_height;
  if (full_source && destination_width == source_width && destination_height == source_height) {
    backend.CopyTexture2D(source, destination, command_context);
    return;
  }

  const geometry_kernels::CropResizePushConstants push{
      .source_width       = source_width,
      .source_height      = source_height,
      .crop_x             = crop.x,
      .crop_y             = crop.y,
      .crop_width         = crop.width,
      .crop_height        = crop.height,
      .destination_width  = destination_width,
      .destination_height = destination_height,
      .scale_x            = static_cast<float>(crop.width) / static_cast<float>(destination_width),
      .scale_y = static_cast<float>(crop.height) / static_cast<float>(destination_height),
  };
  const std::array<VulkanImageBinding, 2> images = {
      VulkanImageBinding{geometry_kernels::kSourceBinding, &source},
      VulkanImageBinding{geometry_kernels::kDestinationBinding, &destination},
  };
  RecordKernelDispatch(
      backend, command_context, geometry_kernels::CropResizeLinearDescriptor(), images,
      PushConstantBytes(push),
      WorkgroupsCovering(destination.Width(), destination.Height(), geometry_kernels::kGroupSize,
                         geometry_kernels::kGroupSize));
}

}  // namespace alcedo::vulkan::geometry

#endif  // HAVE_VULKAN
