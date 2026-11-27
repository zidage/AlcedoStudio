//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_VULKAN

#include <cstdint>
#include <stdexcept>

#include "edit/runtime/frame_scene_binding.hpp"
#include "edit/runtime/texture_format.hpp"
#include "edit/runtime/vulkan/vulkan_render_device.hpp"

namespace alcedo {

/**
 * @brief The Vulkan view of a frame-local scene location (phase VK8): the form of
 * `OpenClSceneView`.
 *
 * A cached image (Develop, Camera Color) is an RGBA32F storage image (@ref image). A scene-work
 * member is an RGBA32F row-major buffer (@ref buffer, its device address). Exactly one of the two
 * is set. The view does not own the storage; the workspace keeps it for the render.
 */
struct VulkanSceneView {
  const VulkanBackend::Texture2D* image  = nullptr;
  VkDeviceAddress                 buffer = 0;
  std::uint32_t                   width  = 0;
  std::uint32_t                   height = 0;

  [[nodiscard]] auto              IsBuffer() const -> bool { return buffer != 0; }
};

/**
 * @brief The view of @p binding in the workspace of @p device.
 * @throws std::runtime_error For an empty scene-work member, a missing cached image, or a cached
 *         image that is not RGBA32F.
 */
[[nodiscard]] inline auto VulkanBindScene(VulkanRenderDevice&      device,
                                          const FrameSceneBinding& binding) -> VulkanSceneView {
  if (binding.IsWorkImage()) {
    auto& member = device.Workspace().SceneWork().Member(binding.member);
    if (member.Empty()) {
      throw std::runtime_error("Vulkan scene work member is empty");
    }
    return VulkanSceneView{nullptr, member.Storage().DeviceAddress(), member.Width(),
                           member.Height()};
  }
  auto* lease = device.Workspace().Images().Find(binding.graph_id);
  if (lease == nullptr || lease->Empty()) {
    throw std::runtime_error("Vulkan scene image is missing");
  }
  const auto& texture = lease->Texture();
  if (texture.Format() != TextureFormat::Rgba32f) {
    throw std::runtime_error("Vulkan scene image is not RGBA32F");
  }
  return VulkanSceneView{&texture, 0, texture.Width(), texture.Height()};
}

}  // namespace alcedo

#endif  // HAVE_VULKAN
