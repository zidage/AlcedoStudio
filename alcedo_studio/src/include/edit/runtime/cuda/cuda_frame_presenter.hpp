//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "edit/runtime/cuda/cuda_backend.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/frame_presenter.hpp"

namespace alcedo {

/**
 * @brief CUDA DRT texture present and host download. No CPU image-processing path.
 */
template <>
struct FramePresenter<CudaBackend> {
  static void Present(CudaRenderDevice& device, const GraphValueId& output_id, IFrameSink& sink,
                      const FrameCompletionSubmission& submission,
                      const ViewerDisplayConfig&       display_config);
  static auto Download(CudaRenderDevice& device, const GraphValueId& output_id)
      -> std::shared_ptr<ImageBuffer>;
};

}  // namespace alcedo
