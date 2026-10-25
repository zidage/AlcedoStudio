//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <memory>
#include <stdexcept>

#include "edit/graph/graph_ids.hpp"
#include "ui/edit_viewer/frame_sink.hpp"
#include "ui/edit_viewer/mask_coverage_sink.hpp"

namespace alcedo {

class ImageBuffer;

/**
 * @brief Native present and host download for one render backend.
 *
 * Specializations own GPU copies, signaling, and sink mapping: the photo frame (IFrameSink) and
 * the AI Mask coverage display (IMaskCoverageSink, SM8.5). The primary template fails explicitly
 * and does not substitute another backend.
 *
 * @tparam Backend Render backend.
 */
template <class Backend>
struct FramePresenter {
  template <class Device>
  static void Present(Device&, const GraphValueId&, IFrameSink&, const FrameCompletionSubmission&,
                      const ViewerDisplayConfig&) {
    throw std::runtime_error("FramePresenter: present is not implemented for this backend");
  }

  template <class Device, class Target>
  static auto PresentMaskCoverage(Device&, const Target&, const MaskCoveragePresentation&,
                                  IMaskCoverageSink&) -> MaskCoveragePublication {
    throw std::runtime_error(
        "FramePresenter: coverage presentation is not implemented for this backend");
  }

  template <class Device>
  static auto Download(Device&, const GraphValueId&) -> std::shared_ptr<ImageBuffer> {
    throw std::runtime_error("FramePresenter: host download is not implemented for this backend");
  }
};

}  // namespace alcedo
