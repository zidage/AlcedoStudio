//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_adjustment_pipeline.hpp"

#include <json.hpp>
#include <optional>
#include <utility>

#include "edit/operators/op_base.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"

namespace alcedo {
namespace {

auto FieldSpec(const std::string& field_key)
    -> std::optional<std::pair<PipelineStageName, OperatorType>> {
  if (field_key == "exposure") {
    return {{PipelineStageName::Basic_Adjustment, OperatorType::EXPOSURE}};
  }
  if (field_key == "contrast") {
    return {{PipelineStageName::Basic_Adjustment, OperatorType::CONTRAST}};
  }
  if (field_key == "white" || field_key == "whites") {
    return {{PipelineStageName::Basic_Adjustment, OperatorType::WHITE}};
  }
  if (field_key == "black" || field_key == "blacks") {
    return {{PipelineStageName::Basic_Adjustment, OperatorType::BLACK}};
  }
  if (field_key == "shadows") {
    return {{PipelineStageName::Basic_Adjustment, OperatorType::SHADOWS}};
  }
  if (field_key == "highlights") {
    return {{PipelineStageName::Basic_Adjustment, OperatorType::HIGHLIGHTS}};
  }
  if (field_key == "curve") {
    return {{PipelineStageName::Basic_Adjustment, OperatorType::CURVE}};
  }
  if (field_key == "saturation") {
    return {{PipelineStageName::Color_Adjustment, OperatorType::SATURATION}};
  }
  if (field_key == "vibrance") {
    return {{PipelineStageName::Color_Adjustment, OperatorType::VIBRANCE}};
  }
  if (field_key == "hls" || field_key == "HLS") {
    return {{PipelineStageName::Color_Adjustment, OperatorType::HLS}};
  }
  if (field_key == "color_wheel") {
    return {{PipelineStageName::Color_Adjustment, OperatorType::COLOR_WHEEL}};
  }
  if (field_key == "lut" || field_key == "ocio_lmt") {
    return {{PipelineStageName::Color_Adjustment, OperatorType::LMT}};
  }
  if (field_key == "clarity") {
    return {{PipelineStageName::Detail_Adjustment, OperatorType::CLARITY}};
  }
  if (field_key == "sharpen") {
    return {{PipelineStageName::Detail_Adjustment, OperatorType::SHARPEN}};
  }
  if (field_key == "odt") {
    return {{PipelineStageName::Output_Transform, OperatorType::ODT}};
  }
  if (field_key == "film_grain") {
    return {{PipelineStageName::Output_Transform, OperatorType::FILM_GRAIN}};
  }
  if (field_key == "halation") {
    return {{PipelineStageName::Output_Transform, OperatorType::HALATION}};
  }
  if (field_key == "crop_rotate") {
    return {{PipelineStageName::Geometry_Adjustment, OperatorType::CROP_ROTATE}};
  }
  if (field_key == "raw_decode") {
    return {{PipelineStageName::Image_Loading, OperatorType::RAW_DECODE}};
  }
  if (field_key == "lens_calib") {
    return {{PipelineStageName::Image_Loading, OperatorType::LENS_CALIBRATION}};
  }
  if (field_key == "color_temp") {
    return {{PipelineStageName::To_WorkingSpace, OperatorType::COLOR_TEMP}};
  }
  return std::nullopt;
}

auto EmbeddedEnabled(const nlohmann::json& params) -> bool {
  if (params.is_object() && params.contains("enabled") && params["enabled"].is_boolean()) {
    return params["enabled"].get<bool>();
  }
  if (params.is_object() && params.size() == 1 && params.begin().value().is_object()) {
    const auto& nested = params.begin().value();
    if (nested.contains("enabled") && nested["enabled"].is_boolean()) {
      return nested["enabled"].get<bool>();
    }
  }
  return true;
}

auto LooksLikeFullPipelineParams(const nlohmann::json& params) -> bool {
  if (!params.is_object()) {
    return false;
  }
  return params.contains("Image Loading") || params.contains("Basic Adjustment") ||
         params.contains("Color Adjustment") || params.contains("Output Transform");
}

auto ApplyPatch(CPUPipelineExecutor& executor, const EditorAdjustmentPatch& patch,
                std::string* error) -> bool {
  const auto spec = FieldSpec(patch.field_key);
  if (!spec.has_value()) {
    if (error) {
      *error = "Unknown editor adjustment field: " + patch.field_key;
    }
    return false;
  }
  if (patch.params_json.empty()) {
    return true;
  }
  const auto params = nlohmann::json::parse(patch.params_json);
  if (!params.is_object()) {
    if (error) {
      *error = "Editor adjustment params must be a JSON object";
    }
    return false;
  }
  auto& stage   = executor.GetStage(spec->first);
  auto& globals = executor.GetGlobalParams();
  stage.SetOperator(spec->second, params, globals);
  stage.EnableOperator(spec->second, EmbeddedEnabled(params), globals);
  return true;
}

}  // namespace

auto ApplyEditorAdjustmentSnapshot(CPUPipelineExecutor&                  executor,
                                   const EditorRenderAdjustmentSnapshot& snapshot,
                                   std::string*                          error) -> bool {
  try {
    if (!snapshot.patches.empty()) {
      for (const auto& patch : snapshot.patches) {
        if (!ApplyPatch(executor, patch, error)) {
          return false;
        }
      }
      return true;
    }
    if (snapshot.params_json.empty()) {
      return true;
    }

    const auto params = nlohmann::json::parse(snapshot.params_json);
    if (LooksLikeFullPipelineParams(params)) {
      executor.ImportPipelineParams(params);
      return true;
    }
    if (FieldSpec(snapshot.fingerprint).has_value()) {
      return ApplyPatch(
          executor, EditorAdjustmentPatch{snapshot.fingerprint, snapshot.params_json, true}, error);
    }
    return true;
  } catch (const std::exception& ex) {
    if (error) {
      *error = ex.what();
    }
    return false;
  }
}

}  // namespace alcedo
