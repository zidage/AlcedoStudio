//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "edit/graph/drt_node_model.hpp"
#include "ui/edit_viewer/frame_sink.hpp"

namespace alcedo {

/**
 * @brief Map DRT payload encoding fields to the viewer display configuration.
 *
 * Native present still copies through the backend FramePresenter. This helper
 * does not allocate GPU memory.
 */
[[nodiscard]] inline auto ViewerDisplayConfigFromDrt(const DrtPayload& payload)
    -> ViewerDisplayConfig {
  ViewerDisplayConfig config;
  switch (payload.encoding_space) {
    case DrtColorSpace::Rec2020:
      config.encoding_space = ColorUtils::ColorSpace::REC2020;
      break;
    case DrtColorSpace::P3D65:
      config.encoding_space = ColorUtils::ColorSpace::P3_D65;
      break;
    case DrtColorSpace::Rec709:
    default:
      config.encoding_space = ColorUtils::ColorSpace::REC709;
      break;
  }
  switch (payload.encoding_eotf) {
    case DrtEotf::Linear:
      config.encoding_eotf = ColorUtils::EOTF::LINEAR;
      break;
    case DrtEotf::St2084:
      config.encoding_eotf = ColorUtils::EOTF::ST2084;
      break;
    case DrtEotf::Hlg:
      config.encoding_eotf = ColorUtils::EOTF::HLG;
      break;
    case DrtEotf::Gamma26:
      config.encoding_eotf = ColorUtils::EOTF::GAMMA_2_6;
      break;
    case DrtEotf::Bt1886:
      config.encoding_eotf = ColorUtils::EOTF::BT1886;
      break;
    case DrtEotf::Gamma18:
      config.encoding_eotf = ColorUtils::EOTF::GAMMA_1_8;
      break;
    case DrtEotf::Gamma22:
    default:
      config.encoding_eotf = ColorUtils::EOTF::GAMMA_2_2;
      break;
  }
  config.peak_luminance = payload.peak_luminance;
  return config;
}

}  // namespace alcedo
