//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_adjustment_pipeline.hpp"

#include <array>
#include <json.hpp>
#include <optional>
#include <string_view>
#include <utility>

namespace alcedo {
namespace {

// Every accepted field key, including the aliases that panels and older history rows use.
constexpr std::array<std::pair<std::string_view, EditorAdjustmentField>, 26> kFieldKeys = {{
    {"exposure", EditorAdjustmentField::Exposure},
    {"contrast", EditorAdjustmentField::Contrast},
    {"white", EditorAdjustmentField::Whites},
    {"whites", EditorAdjustmentField::Whites},
    {"black", EditorAdjustmentField::Blacks},
    {"blacks", EditorAdjustmentField::Blacks},
    {"shadows", EditorAdjustmentField::Shadows},
    {"highlights", EditorAdjustmentField::Highlights},
    {"curve", EditorAdjustmentField::Curve},
    {"saturation", EditorAdjustmentField::Saturation},
    {"vibrance", EditorAdjustmentField::Vibrance},
    {"tint", EditorAdjustmentField::Tint},
    {"hls", EditorAdjustmentField::Hls},
    {"HLS", EditorAdjustmentField::Hls},
    {"color_wheel", EditorAdjustmentField::ColorWheel},
    {"lut", EditorAdjustmentField::Lut},
    {"ocio_lmt", EditorAdjustmentField::Lut},
    {"clarity", EditorAdjustmentField::Clarity},
    {"sharpen", EditorAdjustmentField::Sharpen},
    {"odt", EditorAdjustmentField::Drt},
    {"film_grain", EditorAdjustmentField::FilmGrain},
    {"halation", EditorAdjustmentField::Halation},
    {"crop_rotate", EditorAdjustmentField::CropRotate},
    {"raw_decode", EditorAdjustmentField::RawDecode},
    {"lens_calib", EditorAdjustmentField::LensCalibration},
    {"color_temp", EditorAdjustmentField::ColorTemperature},
}};

void RenameJsonKeyIfAbsent(nlohmann::json& params, const char* from, const char* to) {
  if (params.contains(from) && !params.contains(to)) {
    params[to] = params.at(from);
    params.erase(from);
  }
}

}  // namespace

auto ResolveEditorAdjustmentField(const std::string& field_key)
    -> std::optional<EditorAdjustmentField> {
  for (const auto& [key, field] : kFieldKeys) {
    if (key == field_key) {
      return field;
    }
  }
  return std::nullopt;
}

auto EditorAdjustmentDocumentParamsFromWrite(const std::string& field_key, nlohmann::json params)
    -> nlohmann::json {
  if (field_key == "exposure") {
    RenameJsonKeyIfAbsent(params, "exposure", "exposure_ev");
    RenameJsonKeyIfAbsent(params, "value", "exposure_ev");
  }
  if (field_key == "lut") {
    RenameJsonKeyIfAbsent(params, "ocio_lmt", "cube_path");
  }
  return params;
}

}  // namespace alcedo
