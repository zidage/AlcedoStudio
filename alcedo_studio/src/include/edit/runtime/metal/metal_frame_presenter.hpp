//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "edit/runtime/basic_render_device.hpp"
#include "edit/runtime/frame_presenter.hpp"
#include "edit/runtime/metal/metal_backend.hpp"
#include "image/image_buffer.hpp"

namespace alcedo {

/**
 * @brief Metal DRT texture present and host download. No CPU image-processing path.
 *
 * Present submits the workspace MTLTexture to the sink. Host download runs only
 * when the product path requested an export or test ImageBuffer.
 */
template <>
struct FramePresenter<MetalBackend> {
  static void Present(BasicRenderDevice<MetalBackend>& device, const GraphValueId& output_id,
                      IFrameSink& sink, const FrameCompletionSubmission& submission,
                      const ViewerDisplayConfig& display_config);
  static auto Download(BasicRenderDevice<MetalBackend>& device, const GraphValueId& output_id)
      -> std::shared_ptr<ImageBuffer>;
};

}  // namespace alcedo
