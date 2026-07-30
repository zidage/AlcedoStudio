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

void MergeJsonPatch(nlohmann::json& target, const nlohmann::json& patch) {
  if (!target.is_object() || !patch.is_object()) {
    target = patch;
    return;
  }

  for (const auto& [key, value] : patch.items()) {
    if (target.contains(key) && target[key].is_object() && value.is_object()) {
      MergeJsonPatch(target[key], value);
    } else {
      target[key] = value;
    }
  }
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
  const auto patch_params = patch.params_json.empty() ? nlohmann::json::object()
                                                      : nlohmann::json::parse(patch.params_json);
  if (!patch_params.is_object()) {
    if (error) {
      *error = "Editor adjustment params must be a JSON object";
    }
    return false;
  }
  auto  params  = patch_params;
  auto& stage   = executor.GetStage(spec->stage_name);
  auto& globals = executor.GetGlobalParams();
  if (const auto current = stage.GetOperator(spec->operator_type);
      current.has_value() && current.value() != nullptr && current.value()->op_) {
    // Cumulative editor snapshots carry partial operator JSON. Complete it with the operator's
    // canonical parameters before PipelineStage compares the values, so omitted runtime/input
    // fields do not turn an unchanged operator into a cache invalidation.
    auto canonical_params = current.value()->op_->GetParams();
    MergeJsonPatch(canonical_params, params);
    params = std::move(canonical_params);
  }
  stage.SetOperator(spec->operator_type, params, globals);
  const bool has_embedded_enabled =
      patch_params.contains("enabled") ||
      (patch_params.size() == 1 && patch_params.begin().value().is_object());
  const bool enabled = has_embedded_enabled ? EmbeddedEnabled(patch_params) : patch.enabled;
  stage.EnableOperator(spec->operator_type, enabled, globals);
  return true;
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

auto ReadEditorAdjustmentOperatorState(CPUPipelineExecutor& executor, const std::string& field_key,
                                       EditorAdjustmentOperatorState* state, std::string* error)
    -> bool {
  try {
    const auto spec = FieldSpec(field_key);
    if (!spec.has_value()) {
      if (error) *error = "Unknown editor adjustment field: " + field_key;
      return false;
    }
    if (state == nullptr) {
      if (error) *error = "Editor adjustment state output is null";
      return false;
    }
    const auto entry = executor.GetStage(spec->stage_name).GetOperator(spec->operator_type);
    if (!entry.has_value() || *entry == nullptr || !(*entry)->op_) {
      *state = {};
      return true;
    }
    state->params  = (*entry)->op_->GetParams();
    state->enabled = (*entry)->enable_;
    return true;
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }
}

auto ApplyEditorAdjustmentOperatorState(CPUPipelineExecutor&                 executor,
                                        const EditorAdjustmentFieldSpec&     spec,
                                        const EditorAdjustmentOperatorState& state,
                                        std::string*                         error) -> bool {
  try {
    auto& stage   = executor.GetStage(spec.stage_name);
    auto& globals = executor.GetGlobalParams();
    if (state.params.is_object()) {
      stage.SetOperator(spec.operator_type, state.params, globals);
    }
    stage.EnableOperator(spec.operator_type, state.enabled, globals);
    return true;
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }
}

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

void DisableEditorGeometryOperatorForOverlay(CPUPipelineExecutor& executor) {
  auto& stage   = executor.GetStage(PipelineStageName::Geometry_Adjustment);
  auto& globals = executor.GetGlobalParams();
  stage.EnableOperator(OperatorType::CROP_ROTATE, false, globals);
}

}  // namespace alcedo
