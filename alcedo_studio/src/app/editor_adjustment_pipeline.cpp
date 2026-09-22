//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_adjustment_pipeline.hpp"

#include <json.hpp>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "app/editor_pipeline_command_service.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/edit_commit.hpp"
#include "edit/history/pipeline_edit_batch.hpp"
#include "edit/operators/op_base.hpp"
#include "edit/pipeline/default_pipeline_params.hpp"
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

/// Keys that must survive a defaults reset for image-local identity / as-shot baseline.
auto IsImageLocalParamKey(std::string_view field_key, std::string_view key) -> bool {
  if (field_key == "crop_rotate") {
    return key == "source_size";
  }
  if (field_key == "raw_decode") {
    // Persisted inherent RAW context (plan §4.4 / §4.7). decode_res is one-shot and
    // is not preserved as "user edit" either — defaults omit it.
    return key != "method" && key != "highlights_reconstruct" && key != "use_camera_wb" &&
           key != "user_wb" && key != "backend" && key != "decode_res" && key != "gpu_backend";
  }
  if (field_key == "color_temp") {
    return key == "as_shot_cct" || key == "as_shot_tint" || key == "resolved_cct" ||
           key == "resolved_tint";
  }
  if (field_key == "lens_calib") {
    return key == "cam_maker" || key == "cam_model" || key == "lens_maker" || key == "lens_model" ||
           key == "focal_length_mm" || key == "aperture_f_number" || key == "distance_m" ||
           key == "focal_35mm_mm" || key == "crop_factor_hint" || key == "lens_profile_db_path";
  }
  return false;
}

auto ExtractInnerObject(const nlohmann::json& params, std::string_view preferred_key)
    -> nlohmann::json {
  if (!params.is_object()) return nlohmann::json::object();
  if (params.contains(preferred_key) && params[std::string(preferred_key)].is_object()) {
    return params[std::string(preferred_key)];
  }
  if (params.size() == 1 && params.begin().value().is_object()) {
    return params.begin().value();
  }
  return params;
}

auto MergePreservingImageLocal(const nlohmann::json& current_params,
                               const nlohmann::json& default_params, std::string_view field_key)
    -> nlohmann::json {
  nlohmann::json result = default_params;
  if (!current_params.is_object()) return result;

  // Defaults and GetParams both use a single top-level script key for most ops.
  auto merge_objects = [&](nlohmann::json& target_obj, const nlohmann::json& source_obj) {
    if (!target_obj.is_object() || !source_obj.is_object()) return;
    for (const auto& [key, value] : source_obj.items()) {
      if (IsImageLocalParamKey(field_key, key)) {
        target_obj[key] = value;
      }
    }
  };

  if (field_key == "raw_decode") {
    auto& target_inner = result["raw"];
    if (!target_inner.is_object()) target_inner = nlohmann::json::object();
    merge_objects(target_inner, ExtractInnerObject(current_params, "raw"));
    return result;
  }
  if (field_key == "color_temp") {
    auto& target_inner = result["color_temp"];
    if (!target_inner.is_object()) target_inner = nlohmann::json::object();
    merge_objects(target_inner, ExtractInnerObject(current_params, "color_temp"));
    return result;
  }
  if (field_key == "lens_calib") {
    auto& target_inner = result["lens_calib"];
    if (!target_inner.is_object()) target_inner = nlohmann::json::object();
    merge_objects(target_inner, ExtractInnerObject(current_params, "lens_calib"));
    return result;
  }
  if (field_key == "crop_rotate") {
    auto& target_inner = result["crop_rotate"];
    if (!target_inner.is_object()) target_inner = nlohmann::json::object();
    merge_objects(target_inner, ExtractInnerObject(current_params, "crop_rotate"));
    return result;
  }

  // Non-image-local fields: pure default replace.
  return result;
}

auto DefaultParamsForField(std::string_view field_key) -> nlohmann::json {
  using namespace pipeline_defaults;
  if (field_key == "raw_decode") return MakeDefaultRawDecodeParams();
  if (field_key == "lens_calib") return MakeDefaultLensCalibParams();
  if (field_key == "color_temp") {
    return nlohmann::json{{"color_temp",
                           {{"mode", "as_shot"},
                            {"cct", 6500.0f},
                            {"tint", 0.0f},
                            {"custom_cct", 6500.0f},
                            {"custom_tint", 0.0f},
                            {"as_shot_cct", 6500.0f},
                            {"as_shot_tint", 0.0f}}}};
  }
  const auto baseline = MakeCleanBaselineAdjustableParams();
  // MakeCleanBaselineAdjustableParams keys match field names for most fields.
  std::string key(field_key);
  if (key == "hls") key = "HLS";
  if (key == "lut") key = "ocio_lmt";
  if (baseline.contains(key)) return baseline.at(key);
  return nlohmann::json::object();
}

auto DefaultEnabledForField(std::string_view field_key) -> bool {
  if (field_key == "crop_rotate") return false;
  if (field_key == "lens_calib") return pipeline_defaults::kCleanBaselineLensCalibEnabled;
  return true;
}

constexpr const char* kCurrentPanelFields[] = {
    "exposure", "contrast", "white",   "black",     "shadows",    "highlights", "curve",
    "saturation", "vibrance", "tint", "hls",       "color_wheel", "lut",
    "clarity",    "sharpen",  "odt",  "film_grain", "halation",   "crop_rotate", "raw_decode",
    "lens_calib", "color_temp"};

void RenameJsonKeyIfAbsent(nlohmann::json& params, const char* from, const char* to) {
  if (params.contains(from) && !params.contains(to)) {
    params[to] = params.at(from);
    params.erase(from);
  }
}

auto CpuParamsFromModelJson(const std::string& field_key, nlohmann::json params) -> nlohmann::json {
  if (field_key == "exposure") {
    RenameJsonKeyIfAbsent(params, "exposure_ev", "exposure");
    RenameJsonKeyIfAbsent(params, "value", "exposure");
  }
  if (field_key == "lut") {
    RenameJsonKeyIfAbsent(params, "cube_path", "ocio_lmt");
  }
  return params;
}

// Stage-table helpers used only by ApplyVersionHeadToLivePipeline and
// RemirrorCurrentPanelFromDocument until G10.3 removes Version replay into stages.
struct StageOperatorState {
  nlohmann::json params  = nullptr;
  bool           enabled = false;
};

auto ReadStageOperatorState(CPUPipelineExecutor& executor, const EditorAdjustmentFieldSpec& spec)
    -> StageOperatorState {
  const auto entry = executor.GetStage(spec.stage_name).GetOperator(spec.operator_type);
  if (!entry.has_value() || *entry == nullptr || !(*entry)->op_) {
    return {};
  }
  return {(*entry)->op_->GetParams(), (*entry)->enable_};
}

void ApplyStageOperatorState(CPUPipelineExecutor& executor, const EditorAdjustmentFieldSpec& spec,
                             const StageOperatorState& state) {
  auto& stage   = executor.GetStage(spec.stage_name);
  auto& globals = executor.GetGlobalParams();
  if (state.params.is_object()) {
    stage.SetOperator(spec.operator_type, state.params, globals);
  }
  stage.EnableOperator(spec.operator_type, state.enabled, globals);
}

/// Writes the after-values of one typed-batch commit into the stage table.
auto ApplyCommitAfterValuesToStages(CPUPipelineExecutor& executor, const EditCommit& commit,
                                    std::string* error) -> bool {
  if (!IsPipelineEditBatchJson(commit.GetPayloadJSON())) {
    if (error) *error = "Commit payload is not a typed batch";
    return false;
  }
  const auto batch = PipelineEditBatch::FromJSON(commit.GetPayloadJSON());
  for (const auto& change : batch.changes) {
    const auto* parameter = std::get_if<SetParameterChange>(&change);
    if (parameter == nullptr) {
      continue;
    }
    const auto spec = FieldSpec(parameter->target.field_key);
    if (!spec.has_value()) {
      continue;
    }
    ApplyStageOperatorState(
        executor, *spec,
        {CpuParamsFromModelJson(parameter->target.field_key, parameter->after_value),
         parameter->after_enabled});
  }
  return true;
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

auto ResetEditableOperatorsToDefaultsPreservingImageLocal(CPUPipelineExecutor& executor,
                                                          std::string*         error) -> bool {
  try {
    static constexpr std::string_view kFields[] = {
        "exposure",    "contrast",  "white",       "black",      "shadows",   "highlights",
        "curve",       "saturation","vibrance",    "hls",        "color_wheel","lut",
        "clarity",     "sharpen",   "odt",         "film_grain", "halation",  "crop_rotate",
        "raw_decode",  "lens_calib","color_temp"};

    for (const auto field_key_view : kFields) {
      const std::string field_key(field_key_view);
      const auto        spec = FieldSpec(field_key);
      if (!spec.has_value()) continue;

      const auto current = ReadStageOperatorState(executor, *spec);
      ApplyStageOperatorState(
          executor, *spec,
          {MergePreservingImageLocal(current.params, DefaultParamsForField(field_key), field_key),
           DefaultEnabledForField(field_key)});
    }
    return true;
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }
}

auto RemirrorCurrentPanelFromDocument(CPUPipelineExecutor& executor,
                                      const PipelineDocument& document, std::string* error)
    -> bool {
  for (const char* field : kCurrentPanelFields) {
    std::string field_error;
    const auto  target = CompleteCurrentPanelParameterTarget(document, field, &field_error);
    if (!target.has_value()) {
      continue;
    }
    nlohmann::json json;
    if (!ReadEditorParameterJson(document, *target, &json, error)) {
      return false;
    }
    const auto spec = FieldSpec(field);
    if (!spec.has_value()) {
      continue;
    }
    try {
      ApplyStageOperatorState(executor, *spec, {CpuParamsFromModelJson(field, std::move(json)), true});
    } catch (const std::exception& ex) {
      if (error) *error = ex.what();
      return false;
    }
  }
  return true;
}

auto ApplyVersionHeadToLivePipeline(CPUPipelineExecutor&      executor, const CommitGraph& graph,
                                    const head_commit_hash_t& head, std::string* error) -> bool {
  nlohmann::json prior;
  try {
    prior = executor.ExportPipelineParams();
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }

  auto restore = [&]() {
    try {
      executor.ImportPipelineParams(prior);
      executor.SetExecutionStages();
    } catch (...) {
      // Best-effort restore; original error is more useful to the caller.
    }
  };

  try {
    if (!ResetEditableOperatorsToDefaultsPreservingImageLocal(executor, error)) {
      restore();
      return false;
    }
    for (const auto& hash : graph.FirstParentChain(head)) {
      if (!ApplyCommitAfterValuesToStages(executor, graph.GetCommit(hash), error)) {
        restore();
        return false;
      }
    }
    if (const auto document = executor.GpuDagDocument()) {
      if (!RemirrorCurrentPanelFromDocument(executor, *document, error)) {
        restore();
        return false;
      }
    }
    executor.SetExecutionStages();
    return true;
  } catch (const std::exception& ex) {
    restore();
    if (error) *error = ex.what();
    return false;
  }
}

}  // namespace alcedo
