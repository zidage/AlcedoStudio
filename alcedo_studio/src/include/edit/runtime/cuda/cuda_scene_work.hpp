//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <stdexcept>
#include <string>

#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/frame_scene_binding.hpp"

namespace alcedo {

/**
 * @brief Resolve a frame-local scene binding to the CUDA RGBA32F linear texture.
 *
 * Work members come from SceneWorkImagePair. Cached and display images come from
 * GraphImageCache. The returned reference stays valid for the current encode.
 *
 * @throws std::runtime_error when the binding is missing or empty.
 */
[[nodiscard]] inline auto CudaSceneTexture(CudaRenderDevice& device, const FrameSceneBinding& binding)
    -> CudaBackend::Texture2D& {
  if (binding.IsWorkImage()) {
    return device.Workspace().SceneWork().Member(binding.member);
  }
  auto* image = device.Workspace().Images().Find(binding.graph_id);
  if (image == nullptr || image->Empty()) {
    throw std::runtime_error("CUDA scene binding is missing");
  }
  return image->Texture();
}

[[nodiscard]] inline auto CudaSceneWidth(CudaRenderDevice& device, const FrameSceneBinding& binding)
    -> std::uint32_t {
  return CudaSceneTexture(device, binding).Width();
}

[[nodiscard]] inline auto CudaSceneHeight(CudaRenderDevice& device, const FrameSceneBinding& binding)
    -> std::uint32_t {
  return CudaSceneTexture(device, binding).Height();
}

}  // namespace alcedo
