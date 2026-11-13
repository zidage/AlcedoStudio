//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_METAL

#include <stdexcept>

#include "edit/runtime/frame_scene_binding.hpp"
#include "edit/runtime/metal/metal_backend.hpp"

namespace alcedo {

/**
 * @brief Resolve a frame-local scene binding to the Metal RGBA32Float texture.
 *
 * Work members come from SceneWorkImagePair. Cached and display images come from
 * GraphImageCache.
 */
[[nodiscard]] inline auto MetalSceneTexture(MetalRenderDevice& device,
                                            const FrameSceneBinding& binding)
    -> MetalBackend::Texture2D& {
  if (binding.IsWorkImage()) {
    return device.Workspace().SceneWork().Member(binding.member);
  }
  auto* image = device.Workspace().Images().Find(binding.graph_id);
  if (image == nullptr || image->Empty()) {
    throw std::runtime_error("Metal scene binding is missing");
  }
  return image->Texture();
}

[[nodiscard]] inline auto MetalSceneWidth(MetalRenderDevice& device,
                                          const FrameSceneBinding& binding) -> std::uint32_t {
  return MetalSceneTexture(device, binding).Width();
}

[[nodiscard]] inline auto MetalSceneHeight(MetalRenderDevice& device,
                                           const FrameSceneBinding& binding) -> std::uint32_t {
  return MetalSceneTexture(device, binding).Height();
}

}  // namespace alcedo

#endif
