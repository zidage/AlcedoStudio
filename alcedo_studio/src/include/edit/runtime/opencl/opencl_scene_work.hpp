//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include <cstdint>
#include <stdexcept>
#include <string>

#include "edit/runtime/frame_scene_binding.hpp"
#include "edit/runtime/opencl/opencl_backend.hpp"
#include "opencl/opencl_check.hpp"

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

/**
 * @brief Whether a dual-storage scene argument is a kernel read or write.
 *
 * Unused image and buffer slots must bind a dummy whose access matches the
 * kernel qualifier. One dummy object cannot fill both a read_only image and a
 * write_only image in the same dispatch.
 */
enum class OpenClSceneArgAccess : std::uint8_t { Read, Write };

/** @brief 1x1 image view used when a read scene argument is unused. */
[[nodiscard]] inline auto OpenClUnusedSceneReadView(OpenClBackend& backend) -> OpenClSceneView {
  return OpenClSceneView{backend.DummySceneReadImage(), 1, 1, false};
}

/**
 * @brief Bind image, buffer, and storage flag for one dual-storage scene argument.
 *
 * @param access Selects the dummy image and dummy buffer when @p view is the
 *        unused storage kind. Read arguments never share dummy objects with write
 *        arguments.
 */
inline void BindOpenClSceneView(cl_kernel kernel, cl_uint start, const OpenClSceneView& view,
                                OpenClBackend& backend, const char* label,
                                OpenClSceneArgAccess access) {
  const bool write = access == OpenClSceneArgAccess::Write;
  cl_mem     image = view.is_buffer ? (write ? backend.DummySceneWriteImage()
                                             : backend.DummySceneReadImage())
                                    : view.native;
  cl_mem buffer    = view.is_buffer ? view.native
                                    : (write ? backend.DummySceneWriteBuffer()
                                             : backend.DummySceneReadBuffer());
  int    is_buf    = view.is_buffer ? 1 : 0;
  CheckOpenCl(clSetKernelArg(kernel, start, sizeof(cl_mem), &image),
              (std::string(label) + " image").c_str());
  CheckOpenCl(clSetKernelArg(kernel, start + 1, sizeof(cl_mem), &buffer),
              (std::string(label) + " buffer").c_str());
  CheckOpenCl(clSetKernelArg(kernel, start + 2, sizeof(int), &is_buf),
              (std::string(label) + " storage").c_str());
}

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
