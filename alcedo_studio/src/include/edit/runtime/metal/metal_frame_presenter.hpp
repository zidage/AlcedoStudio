//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "edit/runtime/frame_presenter.hpp"
#include "edit/runtime/metal/metal_backend.hpp"
#include "image/image_buffer.hpp"

namespace alcedo {

/**
 * @brief Metal present/download. Fails until the Metal presenter lands.
 *
 * Does not copy to CPU as a stand-in for display, and does not call CUDA present.
 */
template <>
struct FramePresenter<MetalBackend> {
  template <class Device>
  static void Present(Device&, const GraphValueId&, IFrameSink&, const FrameCompletionSubmission&,
                      const ViewerDisplayConfig&) {
    throw std::runtime_error("FramePresenter<MetalBackend>: present is not implemented");
  }

  template <class Device>
  static auto Download(Device&, const GraphValueId&) -> std::shared_ptr<ImageBuffer> {
    throw std::runtime_error("FramePresenter<MetalBackend>: host download is not implemented");
  }
};

}  // namespace alcedo
