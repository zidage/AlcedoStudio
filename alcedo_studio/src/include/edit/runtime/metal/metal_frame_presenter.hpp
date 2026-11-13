//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "edit/runtime/basic_render_device.hpp"
#include "edit/runtime/frame_presenter.hpp"
#include "edit/runtime/mask_coverage_display.hpp"
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
  /**
   * @brief Hand a written coverage display target to the coverage channel of @p sink (SM8.5).
   *
   * Waits for the command buffer of the coverage work, then submits the MTLTexture itself. The
   * frame owner retains the texture and shares the reader identity of @p target, so the
   * workspace does not write the texture again while the viewport displays it. No host readback.
   * @throws std::runtime_error when the command buffer failed.
   */
  static auto PresentMaskCoverage(BasicRenderDevice<MetalBackend>&               device,
                                  const MaskCoverageDisplayTarget<MetalBackend>& target,
                                  const MaskCoveragePresentation&                presentation,
                                  IMaskCoverageSink& sink) -> MaskCoveragePublication;
  static auto Download(BasicRenderDevice<MetalBackend>& device, const GraphValueId& output_id)
      -> std::shared_ptr<ImageBuffer>;
};

}  // namespace alcedo
