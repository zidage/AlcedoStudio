//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "edit/graph/drt_node_model.hpp"
#include "io/image/export_color_profile_config.hpp"
#include "json.hpp"
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

/**
 * @brief Encoding fields from a DRT payload. Does not allocate GPU memory.
 */
[[nodiscard]] inline auto ExportColorProfileFromDrt(const DrtPayload& payload)
    -> ExportColorProfileConfig {
  const auto display = ViewerDisplayConfigFromDrt(payload);
  return {display.encoding_space, display.encoding_eotf, display.peak_luminance};
}

/**
 * @brief Overlay export encoding onto a DRT JSON copy. Does not write the live Model.
 */
inline void OverlayExportColorOnDrtJson(nlohmann::json& json, const ExportColorProfileConfig& color) {
  switch (color.encoding_space) {
    case ColorUtils::ColorSpace::REC2020:
      json["encoding_space"] = "rec2020";
      break;
    case ColorUtils::ColorSpace::P3_D65:
      json["encoding_space"] = "p3_d65";
      break;
    case ColorUtils::ColorSpace::REC709:
    default:
      json["encoding_space"] = "rec709";
      break;
  }
  switch (color.encoding_eotf) {
    case ColorUtils::EOTF::LINEAR:
      json["encoding_eotf"] = "linear";
      break;
    case ColorUtils::EOTF::ST2084:
      json["encoding_eotf"] = "st2084";
      break;
    case ColorUtils::EOTF::HLG:
      json["encoding_eotf"] = "hlg";
      break;
    case ColorUtils::EOTF::GAMMA_2_6:
      json["encoding_eotf"] = "gamma_2_6";
      break;
    case ColorUtils::EOTF::BT1886:
      json["encoding_eotf"] = "bt1886";
      break;
    case ColorUtils::EOTF::GAMMA_1_8:
      json["encoding_eotf"] = "gamma_1_8";
      break;
    case ColorUtils::EOTF::GAMMA_2_2:
    default:
      json["encoding_eotf"] = "gamma_2_2";
      break;
  }
  json["peak_luminance"] = color.peak_luminance;
}

}  // namespace alcedo
