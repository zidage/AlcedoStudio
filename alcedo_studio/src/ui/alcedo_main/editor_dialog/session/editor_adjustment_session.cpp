//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/editor_dialog/session/editor_adjustment_session.hpp"

#include <exception>
#include <optional>
#include <string>
#include <utility>

#include "edit/history/edit_transaction.hpp"
#include "edit/operators/operator_factory.hpp"
#include "ui/alcedo_main/editor_dialog/modules/pipeline_io.hpp"

namespace alcedo::ui {
namespace {

auto StageExportName(PipelineStageName stage) -> std::string {
  switch (stage) {
    case PipelineStageName::Image_Loading:
      return "Image Loading";
    case PipelineStageName::To_WorkingSpace:
      return "To Working Space";
    case PipelineStageName::Basic_Adjustment:
      return "Basic Adjustment";
    case PipelineStageName::Color_Adjustment:
      return "Color Adjustment";
    case PipelineStageName::Detail_Adjustment:
      return "Detail Adjustment";
    case PipelineStageName::Output_Transform:
      return "Output Transform";
    case PipelineStageName::Geometry_Adjustment:
      return "Geometry Adjustment";
    case PipelineStageName::Merged_Stage:
      return "Merged Stage";
    case PipelineStageName::Stage_Count:
      break;
  }
  return "Unknown Stage";
}

auto OperatorScriptName(OperatorType op_type, const nlohmann::json& params) -> std::string {
  if (auto op = OperatorFactory::Instance().Create(op_type, params); op) {
    return op->GetScriptName();
  }

  switch (op_type) {
    case OperatorType::RAW_DECODE:
      return "raw_decode";
    case OperatorType::CROP_ROTATE:
      return "crop_rotate";
    case OperatorType::EXPOSURE:
      return "exposure";
    case OperatorType::CONTRAST:
      return "contrast";
    case OperatorType::WHITE:
      return "white";
    case OperatorType::BLACK:
      return "black";
    case OperatorType::SHADOWS:
      return "shadows";
    case OperatorType::HIGHLIGHTS:
      return "highlights";
    case OperatorType::CURVE:
      return "curve";
    case OperatorType::HLS:
      return "HLS";
    case OperatorType::SATURATION:
      return "saturation";
    case OperatorType::LMT:
      return "ocio_lmt";
    case OperatorType::ODT:
      return "odt";
    case OperatorType::CLARITY:
      return "clarity";
    case OperatorType::SHARPEN:
      return "sharpen";
    case OperatorType::COLOR_WHEEL:
      return "color_wheel";
    case OperatorType::LENS_CALIBRATION:
      return "lens_calib";
    case OperatorType::COLOR_TEMP:
      return "color_temp";
    case OperatorType::FILM_GRAIN:
      return "film_grain";
    case OperatorType::HALATION:
      return "halation";
    default:
      break;
  }
  return OperatorTypeToString(op_type);
}

auto IsLutPathEmpty(const nlohmann::json& params) -> bool {
  if (!params.is_object() || !params.contains("ocio_lmt")) {
    return true;
  }
  try {
    return params.at("ocio_lmt").get<std::string>().empty();
  } catch (...) {
    return true;
  }
}

auto ExtractEmbeddedEnabled(const nlohmann::json& params) -> std::optional<bool> {
  if (!params.is_object()) {
    return std::nullopt;
  }
  if (params.contains("enabled") && params.at("enabled").is_boolean()) {
    return params.at("enabled").get<bool>();
  }
  if (params.size() == 1) {
    const auto& value = params.begin().value();
    if (value.is_object() && value.contains("enabled") && value.at("enabled").is_boolean()) {
      return value.at("enabled").get<bool>();
    }
  }
  return std::nullopt;
}

auto ResolveEnabledForParams(OperatorType op_type, const nlohmann::json& params,
                             bool requested_enabled) -> bool {
  if (!requested_enabled) {
    return false;
  }
  if (op_type == OperatorType::LMT) {
    return !IsLutPathEmpty(params);
  }
  if (const auto embedded_enabled = ExtractEmbeddedEnabled(params); embedded_enabled.has_value()) {
    return *embedded_enabled;
  }
  return true;
}

auto ApplyOperatorParamsToMaterializedHead(const WorkingVersion& working_version,
                                           PipelineStageName stage_name, OperatorType op_type,
                                           const nlohmann::json& params, bool enabled)
    -> std::optional<nlohmann::json> {
  auto head = working_version.GetHeadPipelineParams();
  if (!head.has_value() || !head->is_object()) {
    return std::nullopt;
  }

  const std::string stage_export_name = StageExportName(stage_name);
  auto&             stage_wrapper     = (*head)[stage_export_name];
  if (!stage_wrapper.is_object()) {
    stage_wrapper = nlohmann::json::object();
  }
  auto& stage_ops = stage_wrapper[stage_export_name];
  if (!stage_ops.is_object()) {
    stage_ops = nlohmann::json::object();
  }

  stage_ops[OperatorScriptName(op_type, params)] = {
      {"type", static_cast<int>(op_type)},
      {"enable", ResolveEnabledForParams(op_type, params, enabled)},
      {"params", params},
  };
  return head;
}

}  // namespace

EditorAdjustmentSession::EditorAdjustmentSession(Dependencies dependencies, Callbacks callbacks)
    : dependencies_(std::move(dependencies)), callbacks_(std::move(callbacks)) {}

void EditorAdjustmentSession::Preview(const AdjustmentPreview& preview) { last_preview_ = preview; }

auto EditorAdjustmentSession::Commit(AdjustmentField field) -> CommitResult {
  return Commit(AdjustmentCommit{.field = field});
}

auto EditorAdjustmentSession::Commit(const AdjustmentCommit& commit) -> CommitResult {
  if (commit.policy != CommitPolicy::AppendTransaction || !FieldChanged(commit.field) ||
      dependencies_.working_version == nullptr || dependencies_.state == nullptr ||
      dependencies_.committed_state == nullptr) {
    ScheduleQualityPreview();
    return {.status = CommitStatus::UnchangedOrUnavailable};
  }

  const auto [stage_name, op_type] = FieldSpec(commit.field);
  const auto new_params =
      commit.new_params.value_or(ParamsForField(commit.field, *dependencies_.state));
  const auto before_params =
      commit.old_params.value_or(ParamsForField(commit.field, *dependencies_.committed_state));

  const TransactionType tx_type =
      !before_params.is_null() ? TransactionType::_EDIT : TransactionType::_ADD;
  const bool before_enabled = !before_params.is_null();

  EditTransaction tx{tx_type, op_type, stage_name, before_params, new_params, before_enabled, true};
  if (auto head_params = ApplyOperatorParamsToMaterializedHead(
          *dependencies_.working_version, stage_name, op_type, new_params, true);
      head_params.has_value()) {
    dependencies_.working_version->SetHeadPipelineParams(*head_params);
  }

  dependencies_.working_version->AppendEditTransaction(std::move(tx));
  if (dependencies_.pipeline_guard) {
    dependencies_.pipeline_guard->dirty_ = true;
  }

  CopyFieldState(commit.field, *dependencies_.state, *dependencies_.committed_state);
  if (commit.field == AdjustmentField::CropRotate &&
      callbacks_.mark_full_frame_preview_after_geometry_commit) {
    callbacks_.mark_full_frame_preview_after_geometry_commit();
  }
  if (callbacks_.update_version_ui) {
    callbacks_.update_version_ui();
  }

  if (callbacks_.advance_preview_generation) {
    callbacks_.advance_preview_generation();
  }
  ScheduleQualityPreview();

  return {.status = CommitStatus::Applied};
}

auto EditorAdjustmentSession::LoadFromPipeline() -> bool {
  if (!HasPipeline() || dependencies_.state == nullptr) {
    return false;
  }

  auto [loaded_state, has_loaded_any] = pipeline_io::LoadStateFromPipeline(
      *dependencies_.pipeline_guard->pipeline_, *dependencies_.state);
  if (!has_loaded_any) {
    return false;
  }
  *dependencies_.state = std::move(loaded_state);
  if (dependencies_.committed_state != nullptr) {
    *dependencies_.committed_state = *dependencies_.state;
  }
  return true;
}

auto EditorAdjustmentSession::ReloadFromImportedPipelineParams() -> bool {
  return LoadFromPipeline();
}

auto EditorAdjustmentSession::ReadCurrentOperatorParams(PipelineStageName stage_name,
                                                        OperatorType      op_type) const
    -> std::optional<nlohmann::json> {
  if (!HasPipeline()) {
    return std::nullopt;
  }
  return pipeline_io::ReadCurrentOperatorParams(*dependencies_.pipeline_guard->pipeline_,
                                                stage_name, op_type);
}

auto EditorAdjustmentSession::FieldSpec(AdjustmentField field) const
    -> std::pair<PipelineStageName, OperatorType> {
  return pipeline_io::FieldSpec(field);
}

auto EditorAdjustmentSession::ParamsForField(AdjustmentField        field,
                                             const AdjustmentState& state) const -> nlohmann::json {
  return pipeline_io::ParamsForField(
      field, state, HasPipeline() ? dependencies_.pipeline_guard->pipeline_.get() : nullptr);
}

auto EditorAdjustmentSession::FieldChanged(AdjustmentField field) const -> bool {
  if (dependencies_.state == nullptr || dependencies_.committed_state == nullptr) {
    return false;
  }
  return pipeline_io::FieldChanged(field, *dependencies_.state, *dependencies_.committed_state);
}

auto EditorAdjustmentSession::HasPipeline() const -> bool {
  return dependencies_.pipeline_guard && dependencies_.pipeline_guard->pipeline_;
}

auto EditorAdjustmentSession::Pipeline() const -> CPUPipelineExecutor* {
  return HasPipeline() ? dependencies_.pipeline_guard->pipeline_.get() : nullptr;
}

void EditorAdjustmentSession::ScheduleQualityPreview() const {
  if (callbacks_.schedule_quality_preview) {
    callbacks_.schedule_quality_preview();
  }
  if (callbacks_.schedule_detail_preview_from_viewport) {
    callbacks_.schedule_detail_preview_from_viewport();
  }
}

}  // namespace alcedo::ui
