//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include <stdexcept>
#include <string>

#include "edit/runtime/frame_scene_binding.hpp"
#include "edit/runtime/opencl/opencl_backend.hpp"

namespace alcedo {

/**
 * @brief OpenCL view of a frame-local scene location: cached image or work buffer.
 *
 * Texture2D stays image-backed. Scene-work members are row-major buffers. This
 * type is the Grade/LLF/Neighbor/DRT adapter; it is not a Texture2D union.
 */
struct OpenClSceneView {
  cl_mem        native    = nullptr;
  std::uint32_t width     = 0;
  std::uint32_t height    = 0;
  bool          is_buffer = false;
};

[[nodiscard]] inline auto OpenClBindScene(OpenClRenderDevice& device,
                                          const FrameSceneBinding& binding) -> OpenClSceneView {
  if (binding.IsWorkImage()) {
    auto& image = device.Workspace().SceneWork().Member(binding.member);
    if (image.Empty()) {
      throw std::runtime_error("OpenCL scene work member is empty");
    }
    return OpenClSceneView{image.Native(), image.Width(), image.Height(), true};
  }
  auto* lease = device.Workspace().Images().Find(binding.graph_id);
  if (lease == nullptr || lease->Empty()) {
    throw std::runtime_error("OpenCL scene image is missing");
  }
  const auto& texture = lease->Texture();
  return OpenClSceneView{texture.Native(), texture.Width(), texture.Height(), false};
}

[[nodiscard]] inline auto OpenClSceneWidth(OpenClRenderDevice& device,
                                           const FrameSceneBinding& binding) -> std::uint32_t {
  return OpenClBindScene(device, binding).width;
}

[[nodiscard]] inline auto OpenClSceneHeight(OpenClRenderDevice& device,
                                            const FrameSceneBinding& binding) -> std::uint32_t {
  return OpenClBindScene(device, binding).height;
}

}  // namespace alcedo

#endif
