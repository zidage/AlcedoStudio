//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include "edit/runtime/basic_render_device.hpp"
#include "edit/runtime/frame_presenter.hpp"
#include "edit/runtime/opencl/opencl_backend.hpp"
#include "image/image_buffer.hpp"

namespace alcedo {

/**
 * @brief OpenCL image2d presentation and explicit host download.
 *
 * Present copies the final DRT image into the sink's OpenCL writable image on
 * the product queue. Download is retained for export and test requests only.
 */
template <>
struct FramePresenter<OpenClBackend> {
  static void Present(BasicRenderDevice<OpenClBackend>& device, const GraphValueId& output_id,
                      IFrameSink& sink, const FrameCompletionSubmission& submission,
                      const ViewerDisplayConfig& display_config);
  static auto Download(BasicRenderDevice<OpenClBackend>& device, const GraphValueId& output_id)
      -> std::shared_ptr<ImageBuffer>;
};

}  // namespace alcedo

#endif  // HAVE_OPENCL
