//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_adjustment_pipeline.hpp"

#include <json.hpp>
#include <optional>

#include "edit/operators/op_base.hpp"

namespace alcedo {
namespace {

auto FieldSpec(const std::string& field_key) -> std::optional<EditorAdjustmentFieldSpec> {
  if (field_key == "exposure") {
    return EditorAdjustmentFieldSpec{PipelineStageName::Basic_Adjustment, OperatorType::EXPOSURE};
  }
  if (field_key == "contrast") {
    return EditorAdjustmentFieldSpec{PipelineStageName::Basic_Adjustment, OperatorType::CONTRAST};
  }
  if (field_key == "white" || field_key == "whites") {
    return EditorAdjustmentFieldSpec{PipelineStageName::Basic_Adjustment, OperatorType::WHITE};
  }
  if (field_key == "black" || field_key == "blacks") {
    return EditorAdjustmentFieldSpec{PipelineStageName::Basic_Adjustment, OperatorType::BLACK};
  }
  if (field_key == "shadows") {
    return EditorAdjustmentFieldSpec{PipelineStageName::Basic_Adjustment, OperatorType::SHADOWS};
  }
  if (field_key == "highlights") {
    return EditorAdjustmentFieldSpec{PipelineStageName::Basic_Adjustment, OperatorType::HIGHLIGHTS};
  }
  if (field_key == "curve") {
    return EditorAdjustmentFieldSpec{PipelineStageName::Basic_Adjustment, OperatorType::CURVE};
  }
  if (field_key == "saturation") {
    return EditorAdjustmentFieldSpec{PipelineStageName::Color_Adjustment, OperatorType::SATURATION};
  }
  if (field_key == "vibrance") {
    return EditorAdjustmentFieldSpec{PipelineStageName::Color_Adjustment, OperatorType::VIBRANCE};
  }
  if (field_key == "tint") {
    return EditorAdjustmentFieldSpec{PipelineStageName::Color_Adjustment, OperatorType::TINT};
  }
  if (field_key == "hls" || field_key == "HLS") {
    return EditorAdjustmentFieldSpec{PipelineStageName::Color_Adjustment, OperatorType::HLS};
  }
  if (field_key == "color_wheel") {
    return EditorAdjustmentFieldSpec{PipelineStageName::Color_Adjustment,
                                     OperatorType::COLOR_WHEEL};
  }
  if (field_key == "lut" || field_key == "ocio_lmt") {
    return EditorAdjustmentFieldSpec{PipelineStageName::Color_Adjustment, OperatorType::LMT};
  }
  if (field_key == "clarity") {
    return EditorAdjustmentFieldSpec{PipelineStageName::Detail_Adjustment, OperatorType::CLARITY};
  }
  if (field_key == "sharpen") {
    return EditorAdjustmentFieldSpec{PipelineStageName::Detail_Adjustment, OperatorType::SHARPEN};
  }
  if (field_key == "odt") {
    return EditorAdjustmentFieldSpec{PipelineStageName::Output_Transform, OperatorType::ODT};
  }
  if (field_key == "film_grain") {
    return EditorAdjustmentFieldSpec{PipelineStageName::Output_Transform, OperatorType::FILM_GRAIN};
  }
  if (field_key == "halation") {
    return EditorAdjustmentFieldSpec{PipelineStageName::Output_Transform, OperatorType::HALATION};
  }
  if (field_key == "crop_rotate") {
    return EditorAdjustmentFieldSpec{PipelineStageName::Geometry_Adjustment,
                                     OperatorType::CROP_ROTATE};
  }
  if (field_key == "raw_decode") {
    return EditorAdjustmentFieldSpec{PipelineStageName::Image_Loading, OperatorType::RAW_DECODE};
  }
  if (field_key == "lens_calib") {
    return EditorAdjustmentFieldSpec{PipelineStageName::Image_Loading,
                                     OperatorType::LENS_CALIBRATION};
  }
  if (field_key == "color_temp") {
    return EditorAdjustmentFieldSpec{PipelineStageName::To_WorkingSpace, OperatorType::COLOR_TEMP};
  }
  return std::nullopt;
}

}  // namespace

auto ResolveEditorAdjustmentField(const std::string& field_key)
    -> std::optional<EditorAdjustmentFieldSpec> {
  return FieldSpec(field_key);
}

auto EditorAdjustmentFieldKey(PipelineStageName stage_name, OperatorType operator_type)
    -> std::optional<std::string> {
  const auto matches = [stage_name, operator_type](PipelineStageName expected_stage,
                                                   OperatorType      expected_operator) {
    return stage_name == expected_stage && operator_type == expected_operator;
  };
  if (matches(PipelineStageName::Basic_Adjustment, OperatorType::EXPOSURE)) return "exposure";
  if (matches(PipelineStageName::Basic_Adjustment, OperatorType::CONTRAST)) return "contrast";
  if (matches(PipelineStageName::Basic_Adjustment, OperatorType::WHITE)) return "white";
  if (matches(PipelineStageName::Basic_Adjustment, OperatorType::BLACK)) return "black";
  if (matches(PipelineStageName::Basic_Adjustment, OperatorType::SHADOWS)) return "shadows";
  if (matches(PipelineStageName::Basic_Adjustment, OperatorType::HIGHLIGHTS)) return "highlights";
  if (matches(PipelineStageName::Basic_Adjustment, OperatorType::CURVE)) return "curve";
  if (matches(PipelineStageName::Color_Adjustment, OperatorType::SATURATION)) return "saturation";
  if (matches(PipelineStageName::Color_Adjustment, OperatorType::VIBRANCE)) return "vibrance";
  if (matches(PipelineStageName::Color_Adjustment, OperatorType::TINT)) return "tint";
  if (matches(PipelineStageName::Color_Adjustment, OperatorType::HLS)) return "hls";
  if (matches(PipelineStageName::Color_Adjustment, OperatorType::COLOR_WHEEL)) return "color_wheel";
  if (matches(PipelineStageName::Color_Adjustment, OperatorType::LMT)) return "lut";
  if (matches(PipelineStageName::Detail_Adjustment, OperatorType::CLARITY)) return "clarity";
  if (matches(PipelineStageName::Detail_Adjustment, OperatorType::SHARPEN)) return "sharpen";
  if (matches(PipelineStageName::Output_Transform, OperatorType::ODT)) return "odt";
  if (matches(PipelineStageName::Output_Transform, OperatorType::FILM_GRAIN)) return "film_grain";
  if (matches(PipelineStageName::Output_Transform, OperatorType::HALATION)) return "halation";
  if (matches(PipelineStageName::Geometry_Adjustment, OperatorType::CROP_ROTATE))
    return "crop_rotate";
  if (matches(PipelineStageName::Image_Loading, OperatorType::RAW_DECODE)) return "raw_decode";
  if (matches(PipelineStageName::Image_Loading, OperatorType::LENS_CALIBRATION))
    return "lens_calib";
  if (matches(PipelineStageName::To_WorkingSpace, OperatorType::COLOR_TEMP)) return "color_temp";
  return std::nullopt;
}

namespace {

void RenameJsonKeyIfAbsent(nlohmann::json& params, const char* from, const char* to) {
  if (params.contains(from) && !params.contains(to)) {
    params[to] = params.at(from);
    params.erase(from);
  }
}

}  // namespace

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
