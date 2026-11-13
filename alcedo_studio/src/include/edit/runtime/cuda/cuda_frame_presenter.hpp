//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "edit/runtime/cuda/cuda_backend.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/frame_presenter.hpp"
#include "edit/runtime/mask_coverage_display.hpp"

namespace alcedo {

/**
 * @brief CUDA DRT texture present and host download. No CPU image-processing path.
 */
template <>
struct FramePresenter<CudaBackend> {
  static void Present(CudaRenderDevice& device, const GraphValueId& output_id, IFrameSink& sink,
                      const FrameCompletionSubmission& submission,
                      const ViewerDisplayConfig&       display_config);
  /**
   * @brief Copy a written coverage display target into the coverage channel of @p sink (SM8.5).
   *
   * Device-to-device copy into the mapped CUDA array on the device stream, stream completion,
   * then publication. No host readback.
   * @throws std::runtime_error when the sink has no target (with the presentation-suspended
   *         prefix for a minimized viewport) or the copy fails; the target is then abandoned.
   */
  static auto PresentMaskCoverage(CudaRenderDevice&                             device,
                                  const MaskCoverageDisplayTarget<CudaBackend>& target,
                                  const MaskCoveragePresentation&               presentation,
                                  IMaskCoverageSink& sink) -> MaskCoveragePublication;
  static auto Download(CudaRenderDevice& device, const GraphValueId& output_id)
      -> std::shared_ptr<ImageBuffer>;
};

}  // namespace alcedo
