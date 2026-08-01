//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_adjustment_pipeline.hpp"

#include <json.hpp>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "edit/history/commit_graph.hpp"
#include "edit/history/edit_commit.hpp"
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
    // Partial operator JSON is completed with canonical parameters before
    // PipelineStage compares values, so omitted runtime/input fields do not
    // turn an unchanged operator into a cache invalidation.
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

auto SnapshotTouchesImageLoading(const EditorRenderAdjustmentSnapshot& snapshot) -> bool {
  for (const auto& patch : snapshot.patches) {
    if (patch.field_key == "raw_decode" || patch.field_key == "lens_calib") {
      return true;
    }
  }
  return false;
}

auto ApplyEditorAdjustmentSnapshot(CPUPipelineExecutor&                  executor,
                                   const EditorRenderAdjustmentSnapshot& snapshot,
                                   std::string*                          error) -> bool {
  try {
    for (const auto& patch : snapshot.patches) {
      if (!ApplyPatch(executor, patch, error)) {
        return false;
      }
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

namespace {

/// Keys that must survive a defaults reset for image-local identity / as-shot baseline.
auto IsImageLocalParamKey(std::string_view field_key, std::string_view key) -> bool {
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

auto ApplyOrdinaryPayloadToLive(CPUPipelineExecutor& executor, const OrdinaryEditPayload& payload,
                                bool use_after_value, std::string* error) -> bool {
  const auto field_key =
      EditorAdjustmentFieldKey(payload.stage_name, payload.operator_type);
  if (!field_key.has_value()) {
    if (error) *error = "Unknown operator in history payload";
    return false;
  }
  const auto spec = FieldSpec(*field_key);
  if (!spec.has_value()) {
    if (error) *error = "Unknown editor field for history payload: " + *field_key;
    return false;
  }
  EditorAdjustmentOperatorState state;
  const auto& value = use_after_value ? payload.after_value : payload.before_value;
  state.params      = value.is_null() ? nlohmann::json::object() : value;
  state.enabled     = use_after_value ? payload.after_enabled : payload.before_enabled;
  return ApplyEditorAdjustmentOperatorState(executor, *spec, state, error);
}

}  // namespace

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

      EditorAdjustmentOperatorState current;
      if (!ReadEditorAdjustmentOperatorState(executor, field_key, &current, error)) {
        return false;
      }
      EditorAdjustmentOperatorState next;
      next.params  = MergePreservingImageLocal(current.params, DefaultParamsForField(field_key),
                                               field_key);
      next.enabled = DefaultEnabledForField(field_key);
      if (!ApplyEditorAdjustmentOperatorState(executor, *spec, next, error)) {
        return false;
      }
    }
    return true;
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }
}

auto ApplyHistoryCommitToLivePipeline(CPUPipelineExecutor& executor, const CommitGraph& graph,
                                      const EditCommit& commit, bool use_after_value,
                                      std::string* error) -> bool {
  try {
    if (commit.GetKind() == EditCommitKind::kEdit) {
      return ApplyOrdinaryPayloadToLive(
          executor, OrdinaryEditPayload::FromJSON(commit.GetPayloadJSON()), use_after_value, error);
    }
    if (commit.GetKind() == EditCommitKind::kMerge) {
      const auto payload = MergeEditPayload::FromJSON(commit.GetPayloadJSON());
      for (const auto& field : payload.fields) {
        OrdinaryEditPayload ordinary;
        ordinary.operator_type   = field.operator_type;
        ordinary.stage_name      = field.stage_name;
        ordinary.field_name      = field.field_name;
        ordinary.before_value    = field.before_value;
        ordinary.after_value     = field.resolved_value;
        ordinary.before_enabled  = field.before_enabled;
        ordinary.after_enabled   = field.resolved_enabled;
        if (!ApplyOrdinaryPayloadToLive(executor, ordinary, use_after_value, error)) {
          return false;
        }
      }
      return true;
    }
    if (error) *error = "Unsupported commit kind for live pipeline apply";
    return false;
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }
  (void)graph;
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
      if (!ApplyHistoryCommitToLivePipeline(executor, graph, graph.GetCommit(hash), true, error)) {
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
