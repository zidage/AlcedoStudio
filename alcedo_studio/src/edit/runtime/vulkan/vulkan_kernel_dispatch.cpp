//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_VULKAN

#include "edit/runtime/vulkan/vulkan_kernel_dispatch.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>

#include "vulkan/vulkan_api_counters.hpp"
#include "vulkan/vulkan_context.hpp"

namespace alcedo {
namespace {

/// Set 0 of a kernel has few images; the plan keeps one set of images for each kernel.
constexpr std::size_t kMaxImageBindings = 8;

[[noreturn]] void     ThrowDispatchError(const vulkan::VulkanKernelDescriptor& kernel,
                                         const std::string&                    message) {
  throw std::invalid_argument("RecordKernelDispatch(" + kernel.kernel + "): " + message);
}

/// Checks that @p images has one non-empty texture for each binding of @p kernel.
void RequireImagesOfTheBindings(const vulkan::VulkanKernelDescriptor& kernel,
                                std::span<const VulkanImageBinding>   images) {
  if (images.size() != kernel.bindings.size()) {
    ThrowDispatchError(kernel, std::to_string(images.size()) + " images for " +
                                   std::to_string(kernel.bindings.size()) + " bindings");
  }
  if (images.size() > kMaxImageBindings) {
    ThrowDispatchError(kernel, std::to_string(images.size()) + " images; at most " +
                                   std::to_string(kMaxImageBindings));
  }
  for (const auto& image : images) {
    const bool known = std::any_of(kernel.bindings.begin(), kernel.bindings.end(),
                                   [&](const auto& b) { return b.binding == image.binding; });
    if (!known) {
      ThrowDispatchError(kernel, "the kernel has no binding " + std::to_string(image.binding));
    }
    if (image.texture == nullptr || image.texture->Empty()) {
      ThrowDispatchError(kernel, "empty texture for binding " + std::to_string(image.binding));
    }
  }
  for (std::size_t i = 0; i < images.size(); ++i) {
    for (std::size_t j = i + 1; j < images.size(); ++j) {
      if (images[i].binding == images[j].binding) {
        ThrowDispatchError(kernel,
                           "binding " + std::to_string(images[i].binding) + " has two images");
      }
    }
  }
}

auto DescriptorTypeOf(const vulkan::VulkanKernelDescriptor& kernel, std::uint32_t binding)
    -> VkDescriptorType {
  for (const auto& b : kernel.bindings) {
    if (b.binding == binding) {
      return b.type;
    }
  }
  return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
}

}  // namespace

void RecordKernelDispatch(VulkanBackend& backend, VulkanCommandContext& command_context,
                          const vulkan::VulkanKernelDescriptor& kernel,
                          std::span<const VulkanImageBinding>   images,
                          std::span<const std::byte> push_constants, VulkanWorkgroupCount groups) {
  RequireImagesOfTheBindings(kernel, images);
  if (push_constants.size() != kernel.push_constant_bytes) {
    ThrowDispatchError(kernel, std::to_string(push_constants.size()) +
                                   " push constant bytes; the kernel has " +
                                   std::to_string(kernel.push_constant_bytes));
  }
  if (groups.x == 0 || groups.y == 0 || groups.z == 0) {
    return;
  }

  auto&           context        = vulkan::VulkanContext::Instance();
  const auto&     pipeline       = context.ComputePipelines().Pipeline(kernel);
  const auto&     functions      = context.PipelineDeviceFunctions();

  VkCommandBuffer command_buffer = backend.BeginCommand(command_context);
  for (const auto& image : images) {
    backend.PrepareImage(*image.texture, command_context);
  }
  functions.vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.pipeline);
  if (!images.empty()) {
    std::array<VkDescriptorImageInfo, kMaxImageBindings> image_infos{};
    std::array<VkWriteDescriptorSet, kMaxImageBindings>  writes{};
    for (std::size_t i = 0; i < images.size(); ++i) {
      image_infos[i].imageView   = images[i].texture->View();
      image_infos[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
      writes[i].sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[i].dstBinding       = images[i].binding;
      writes[i].descriptorCount  = 1;
      writes[i].descriptorType   = DescriptorTypeOf(kernel, images[i].binding);
      writes[i].pImageInfo       = &image_infos[i];
    }
    functions.vkCmdPushDescriptorSetKHR(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                        pipeline.layout, 0,
                                        static_cast<std::uint32_t>(images.size()), writes.data());
  }
  if (!push_constants.empty()) {
    functions.vkCmdPushConstants(command_buffer, pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                 static_cast<std::uint32_t>(push_constants.size()),
                                 push_constants.data());
  }
  functions.vkCmdDispatch(command_buffer, groups.x, groups.y, groups.z);
  command_context.NoteWrite();
  vulkan::NoteVulkanDispatch();
}

}  // namespace alcedo

#endif  // HAVE_VULKAN
