//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include "edit/runtime/basic_render_device.hpp"
#include "edit/runtime/frame_presenter.hpp"
#include "edit/runtime/mask_coverage_display.hpp"
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
  /**
   * @brief Copy a written coverage display target into the coverage channel of @p sink (SM8.5).
   *
   * Image copy into the mapped shared OpenCL image on the device queue, completion before the
   * OpenGL release, then publication. No host readback.
   * @throws std::runtime_error when the sink has no target (with the presentation-suspended
   *         prefix for a minimized viewport) or the copy fails; the target is then abandoned.
   */
  static auto PresentMaskCoverage(BasicRenderDevice<OpenClBackend>&               device,
                                  const MaskCoverageDisplayTarget<OpenClBackend>& target,
                                  const MaskCoveragePresentation&                 presentation,
                                  IMaskCoverageSink& sink) -> MaskCoveragePublication;
  static auto Download(BasicRenderDevice<OpenClBackend>& device, const GraphValueId& output_id)
      -> std::shared_ptr<ImageBuffer>;
};

}  // namespace alcedo

#endif  // HAVE_OPENCL
