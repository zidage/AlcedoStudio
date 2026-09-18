//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/graph/legacy_pipeline_importer.hpp"

#include <algorithm>
#include <cmath>

#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/cat02_white_balance_model.hpp"
#include "edit/operators/models/i_operator_model.hpp"
#include "edit/operators/models/json_read.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"

namespace alcedo {

namespace {

constexpr int kLegacyRawDecode         = 0;
constexpr int kLegacyResize            = 1;
constexpr int kLegacyExposure          = 2;
constexpr int kLegacyContrast          = 3;
constexpr int kLegacyWhite             = 4;
constexpr int kLegacyBlack             = 5;
constexpr int kLegacyShadows           = 6;
constexpr int kLegacyHighlights        = 7;
constexpr int kLegacyCurve             = 8;
constexpr int kLegacyHls               = 9;
constexpr int kLegacySaturation        = 10;
constexpr int kLegacyTint              = 11;
constexpr int kLegacyVibrance          = 12;
constexpr int kLegacyCst               = 13;
constexpr int kLegacyToWs              = 14;
constexpr int kLegacyToOutput          = 15;
constexpr int kLegacyLmt               = 16;
constexpr int kLegacyOdt               = 17;
constexpr int kLegacyClarity           = 18;
constexpr int kLegacySharpen           = 19;
constexpr int kLegacyColorWheel        = 20;
constexpr int kLegacyAcesToneMapping   = 21;
constexpr int kLegacyAutoExposure      = 22;
constexpr int kLegacyUnknown           = 23;
constexpr int kLegacyCropRotate        = 24;
constexpr int kLegacyLensCalibration   = 25;
constexpr int kLegacyColorTemp         = 26;
constexpr int kLegacyFilmGrain         = 27;
constexpr int kLegacyHalation          = 28;

struct LegacyOperator {
  int            type    = -1;
  bool           enable  = true;
  nlohmann::json params  = nlohmann::json::object();
};

auto IsSkippedType(int type) -> bool {
  return type == kLegacyResize || type == kLegacyCst || type == kLegacyToWs ||
         type == kLegacyToOutput || type == kLegacyUnknown;
}

auto IsFatalUnknown(int type) -> bool {
  return type == kLegacyAcesToneMapping || type == kLegacyAutoExposure || type < 0 || type > 28;
}

auto NestedOrSelf(const nlohmann::json& params, const char* key) -> nlohmann::json {
  if (params.contains(key)) {
    return params[key];
  }
  return params;
}

void CollectFromObject(const nlohmann::json& object, std::vector<LegacyOperator>& out) {
  if (!object.is_object()) {
    return;
  }
  for (auto it = object.begin(); it != object.end(); ++it) {
    const auto& value = it.value();
    if (!value.is_object()) {
      continue;
    }
    if (value.contains("type")) {
      LegacyOperator op;
      op.type   = value["type"].is_number_integer() ? value["type"].get<int>() : -1;
      op.enable = value.value("enable", true);
      op.params = value.contains("params") ? value["params"] : nlohmann::json::object();
      out.push_back(std::move(op));
    } else {
      CollectFromObject(value, out);
    }
  }
}

auto CollectOperators(const nlohmann::json& stage_json) -> std::vector<LegacyOperator> {
  std::vector<LegacyOperator> operators;
  CollectFromObject(stage_json, operators);
  return operators;
}

auto FindOp(const std::vector<LegacyOperator>& operators, int type) -> const LegacyOperator* {
  for (const auto& op : operators) {
    if (op.type == type) {
      return &op;
    }
  }
  return nullptr;
}

void LoadJsonIfChanged(IOperatorModel* model, const nlohmann::json& json) {
  if (model == nullptr || json.empty()) {
    return;
  }
  const auto before    = model->ToJson();
  const bool was_dirty = model->IsDirty();
  model->LoadJson(json);
  if (!was_dirty && model->ToJson() == before) {
    (void)model->TakeDirtyPatch();
  }
}

void ApplyScalar(IOperatorModel* model, const nlohmann::json& value, const char* new_key) {
  if (model == nullptr) {
    return;
  }
  nlohmann::json json;
  if (value.is_number()) {
    json[new_key] = value.get<float>();
  } else if (value.is_object() && value.contains(new_key)) {
    json[new_key] = value[new_key];
  } else if (value.is_object()) {
    json = value;
  }
  LoadJsonIfChanged(model, json);
}


void ApplyLegacyPercentScalar(IOperatorModel* model, const nlohmann::json& value,
                              const char* new_key) {
  if (model == nullptr) {
    return;
  }
  float legacy_percent = 0.0f;
  if (value.is_number()) {
    legacy_percent = value.get<float>();
  } else if (value.is_object() && value.contains(new_key) && value[new_key].is_number()) {
    legacy_percent = value[new_key].get<float>();
  } else {
    return;
  }
  LoadJsonIfChanged(model,
                    nlohmann::json{{new_key, std::clamp(legacy_percent / 100.0f, 0.0f, 1.0f)}});
}
void ApplyCrop(ImageGeometryModel& geometry, const LegacyOperator& op) {
  const auto crop = NestedOrSelf(op.params, "crop_rotate");
  if (!op.enable || (crop.contains("enabled") && crop["enabled"].is_boolean() && !crop["enabled"].get<bool>())) {
    geometry.SetCropRect(NormalizedRect{});
    geometry.SetRotationDegrees(0.0f);
    geometry.SetExpandToFit(json_util::ReadBool(crop, "expand_to_fit", true));
    return;
  }
  geometry.SetRotationDegrees(json_util::ReadFloat(crop, "angle_degrees", 0.0f));
  geometry.SetExpandToFit(json_util::ReadBool(crop, "expand_to_fit", true));
  const bool enable_crop = json_util::ReadBool(crop, "enable_crop", true);
  if (!enable_crop || !crop.contains("crop_rect")) {
    geometry.SetCropRect(NormalizedRect{});
    return;
  }
  const auto& rect = crop["crop_rect"];
  NormalizedRect crop_rect;
  crop_rect.x = json_util::ReadFloat(rect, "x", 0.0f);
  crop_rect.y = json_util::ReadFloat(rect, "y", 0.0f);
  crop_rect.w = json_util::ReadFloat(rect, "w", 1.0f);
  crop_rect.h = json_util::ReadFloat(rect, "h", 1.0f);
  geometry.SetCropRect(crop_rect);
}

void ApplyDevelop(DevelopParamsModel& develop, const std::vector<LegacyOperator>& operators) {
  auto payload = develop.Params();
  if (const auto* raw = FindOp(operators, kLegacyRawDecode)) {
    const auto raw_params          = NestedOrSelf(raw->params, "raw");
    payload.demosaic_method        = json_util::ReadString(raw_params, "method", payload.demosaic_method);
    payload.highlights_reconstruct =
        json_util::ReadBool(raw_params, "highlights_reconstruct", payload.highlights_reconstruct);
    payload.use_camera_wb = json_util::ReadBool(raw_params, "use_camera_wb", payload.use_camera_wb);
    payload.user_wb       = json_util::ReadFloat(raw_params, "user_wb", payload.user_wb);
  }
  if (const auto* lens = FindOp(operators, kLegacyLensCalibration)) {
    const auto lens_params         = NestedOrSelf(lens->params, "lens_calib");
    payload.lens_enabled           = lens->enable && json_util::ReadBool(lens_params, "enabled", false);
    payload.apply_vignetting       = json_util::ReadBool(lens_params, "apply_vignetting", true);
    payload.apply_distortion       = json_util::ReadBool(lens_params, "apply_distortion", true);
    payload.apply_tca              = json_util::ReadBool(lens_params, "apply_tca", true);
    payload.apply_crop             = json_util::ReadBool(lens_params, "apply_crop", true);
    payload.auto_scale             = json_util::ReadBool(lens_params, "auto_scale", true);
    payload.use_user_scale         = json_util::ReadBool(lens_params, "use_user_scale", false);
    payload.user_scale             = json_util::ReadFloat(lens_params, "user_scale", 1.0f);
    payload.projection_enabled     = json_util::ReadBool(lens_params, "projection_enabled", false);
    payload.target_projection      = json_util::ReadString(lens_params, "target_projection", "unknown");
    payload.lens_profile_db_path =
        json_util::ReadString(lens_params, "lens_profile_db_path", payload.lens_profile_db_path);
  }
  if (const auto* temp = FindOp(operators, kLegacyColorTemp)) {
    const auto color_temp   = NestedOrSelf(temp->params, "color_temp");
    payload.wb_mode         = json_util::ReadString(color_temp, "mode", payload.wb_mode);
    payload.custom_cct      = json_util::ReadFloat(color_temp, "custom_cct", payload.custom_cct);
    if (color_temp.contains("cct")) {
      payload.custom_cct = json_util::ReadFloat(color_temp, "cct", payload.custom_cct);
    }
    payload.custom_tint  = json_util::ReadFloat(color_temp, "custom_tint", payload.custom_tint);
    if (color_temp.contains("tint")) {
      payload.custom_tint = json_util::ReadFloat(color_temp, "tint", payload.custom_tint);
    }
    payload.as_shot_cct  = json_util::ReadFloat(color_temp, "as_shot_cct", payload.as_shot_cct);
    payload.as_shot_tint = json_util::ReadFloat(color_temp, "as_shot_tint", payload.as_shot_tint);
  }
  develop.ReplaceParams(std::move(payload));
}

void ApplyGradeScalar(ColorGradeNodeModel& grade, const OperatorTypeId& type,
                      const LegacyOperator* op, const char* old_key, const char* new_key,
                      bool convert_saturation) {
  if (op == nullptr) {
    return;
  }
  auto* model = grade.FindAdjustmentByType(type);
  if (model == nullptr) {
    return;
  }
  const auto value = NestedOrSelf(op->params, old_key);
  if (convert_saturation && value.is_number()) {
    const float offset     = value.get<float>();
    const float multiplier = std::max(0.0f, 1.0f + offset / 100.0f);
    nlohmann::json json;
    json[new_key] = multiplier;
    LoadJsonIfChanged(model, json);
    return;
  }
  ApplyScalar(model, value, new_key);
}

auto ValidateOperators(const std::vector<LegacyOperator>& operators) -> std::string {
  for (const auto& op : operators) {
    if (IsFatalUnknown(op.type)) {
      return "Unknown legacy operator type: " + std::to_string(op.type);
    }
    if (!IsSkippedType(op.type) && op.type != kLegacyRawDecode && op.type != kLegacyExposure &&
        op.type != kLegacyContrast && op.type != kLegacyWhite && op.type != kLegacyBlack &&
        op.type != kLegacyShadows && op.type != kLegacyHighlights && op.type != kLegacyCurve &&
        op.type != kLegacyHls && op.type != kLegacySaturation && op.type != kLegacyTint &&
        op.type != kLegacyVibrance && op.type != kLegacyLmt && op.type != kLegacyOdt &&
        op.type != kLegacyClarity && op.type != kLegacySharpen && op.type != kLegacyColorWheel &&
        op.type != kLegacyCropRotate && op.type != kLegacyLensCalibration &&
        op.type != kLegacyColorTemp && op.type != kLegacyFilmGrain && op.type != kLegacyHalation) {
      return "Unknown legacy operator type: " + std::to_string(op.type);
    }
  }
  return {};
}

auto LegacyCubePath(const nlohmann::json& params) -> std::string {
  if (params.contains("ocio_lmt")) {
    const auto& value = params["ocio_lmt"];
    if (value.is_string()) {
      return value.get<std::string>();
    }
    if (value.is_object()) {
      const auto nested = json_util::ReadString(value, "ocio_lmt", {});
      if (!nested.empty()) {
        return nested;
      }
      return json_util::ReadString(value, "cube_path", {});
    }
  }
  return json_util::ReadString(params, "cube_path", {});
}

auto ApplyOperators(PipelineDocument& document, const std::vector<LegacyOperator>& operators)
    -> std::string {
  auto* grade = document.PrimaryGrade();
  auto* drt   = document.Drt();
  if (grade == nullptr || drt == nullptr) {
    return "Document is missing grade or DRT";
  }

  if (const auto* crop = FindOp(operators, kLegacyCropRotate)) {
    ApplyCrop(document.Geometry(), *crop);
  }
  if (auto* develop = document.Develop()) {
    ApplyDevelop(develop->Params(), operators);
  }

  ApplyGradeScalar(*grade, type_ids::Exposure(), FindOp(operators, kLegacyExposure), "exposure",
                   "exposure_ev", false);
  ApplyGradeScalar(*grade, type_ids::Contrast(), FindOp(operators, kLegacyContrast), "contrast",
                   "contrast", false);
  ApplyGradeScalar(*grade, type_ids::White(), FindOp(operators, kLegacyWhite), "white", "white",
                   false);
  ApplyGradeScalar(*grade, type_ids::Black(), FindOp(operators, kLegacyBlack), "black", "black",
                   false);
  ApplyGradeScalar(*grade, type_ids::Shadows(), FindOp(operators, kLegacyShadows), "shadows",
                   "shadows", false);
  ApplyGradeScalar(*grade, type_ids::Highlights(), FindOp(operators, kLegacyHighlights),
                   "highlights", "highlights", false);
  ApplyGradeScalar(*grade, type_ids::Saturation(), FindOp(operators, kLegacySaturation),
                   "saturation", "saturation", true);
  ApplyGradeScalar(*grade, type_ids::Vibrance(), FindOp(operators, kLegacyVibrance), "vibrance",
                   "vibrance", false);

  if (const auto* curve = FindOp(operators, kLegacyCurve)) {
    if (auto* model = grade->FindAdjustmentByType(type_ids::Curve())) {
      const auto nested = NestedOrSelf(curve->params, "curve");
      LoadJsonIfChanged(model, nested.is_object() ? nested : curve->params);
    }
  }
  if (const auto* hls = FindOp(operators, kLegacyHls)) {
    if (auto* model = grade->FindAdjustmentByType(type_ids::Hls())) {
      LoadJsonIfChanged(model, NestedOrSelf(hls->params, "HLS"));
    }
  }
  if (const auto* wheel = FindOp(operators, kLegacyColorWheel)) {
    if (auto* model = grade->FindAdjustmentByType(type_ids::ColorWheel())) {
      LoadJsonIfChanged(model, NestedOrSelf(wheel->params, "color_wheel"));
    }
  }
  if (const auto* lmt = FindOp(operators, kLegacyLmt)) {
    if (auto* model = grade->FindAdjustmentByType(type_ids::Lmt())) {
      LoadJsonIfChanged(model, nlohmann::json{{"cube_path", LegacyCubePath(lmt->params)}});
    }
  }
  if (const auto* clarity = FindOp(operators, kLegacyClarity)) {
    auto* model = drt->FindAdjustmentByType(type_ids::Clarity());
    ApplyScalar(model, NestedOrSelf(clarity->params, "clarity"), "clarity");
  }
  if (const auto* sharpen = FindOp(operators, kLegacySharpen)) {
    if (auto* model = drt->FindAdjustmentByType(type_ids::Sharpen())) {
      const auto nested = NestedOrSelf(sharpen->params, "sharpen");
      nlohmann::json json;
      json["amount"] =
          json_util::ReadFloat(nested, "offset", json_util::ReadFloat(nested, "amount", 0.0f));
      json["radius"]    = json_util::ReadFloat(nested, "radius", 3.0f);
      json["threshold"] = json_util::ReadFloat(nested, "threshold", 0.0f);
      LoadJsonIfChanged(model, json);
    }
  }
  if (const auto* grain = FindOp(operators, kLegacyFilmGrain)) {
    auto* model = drt->FindAdjustmentByType(type_ids::FilmGrain());
    const auto nested = NestedOrSelf(grain->params, "film_grain");
    ApplyLegacyPercentScalar(model, nested, "strength");
  }
  if (const auto* halo = FindOp(operators, kLegacyHalation)) {
    auto* model = drt->FindAdjustmentByType(type_ids::Halation());
    const auto nested = NestedOrSelf(halo->params, "halation");
    ApplyLegacyPercentScalar(model, nested, "strength");
  }
  if (const auto* tint = FindOp(operators, kLegacyTint)) {
    // Legacy Tint is not a CUDA grade kernel. Fold it into CAT02 tint_offset so reopen of
    // stored pipelines does not insert an unregistered adjustment type.
    if (auto* cat02 = dynamic_cast<Cat02WhiteBalanceModel*>(
            grade->FindAdjustmentByType(type_ids::Cat02WhiteBalance()))) {
      const auto nested = NestedOrSelf(tint->params, "tint");
      float      value  = 0.0f;
      if (nested.is_number()) {
        value = nested.get<float>();
      } else {
        value = json_util::ReadFloat(nested, "tint", 0.0f);
      }
      cat02->SetTintOffset(value);
    }
  }
  if (const auto* odt = FindOp(operators, kLegacyOdt)) {
    LoadJsonIfChanged(&drt->Params(), NestedOrSelf(odt->params, "odt"));
  }
  return {};
}

}  // namespace

auto LegacyPipelineImporter::Import(const nlohmann::json& stage_json) -> LegacyImportResult {
  LegacyImportResult result;
  auto               document = CreateDefaultPipelineDocument();
  result.error                = ApplyOnto(document, stage_json);
  if (result.error.empty()) {
    result.document = std::move(document);
  }
  return result;
}

auto LegacyPipelineImporter::ApplyOnto(PipelineDocument&     document,
                                       const nlohmann::json& stage_json) -> std::string {
  const auto operators = CollectOperators(stage_json);
  if (const auto error = ValidateOperators(operators); !error.empty()) {
    return error;
  }
  return ApplyOperators(document, operators);
}

}  // namespace alcedo
