//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_pipeline_command_service.hpp"

#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/dcp_camera_profile_binding.hpp"
#include "app/editor_parameter_write.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/film_format.hpp"
#include "edit/graph/graph_validation.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/curve_model.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/operators/models/tone_curve_presets.hpp"
#include "image/dcp_camera_profile.hpp"

namespace alcedo {
namespace {

auto SetError(std::string* error, std::string message) -> bool {
  if (error != nullptr) {
    *error = std::move(message);
  }
  return false;
}

auto JoinValidation(const std::vector<GraphValidationError>& errors) -> std::string {
  std::string text;
  for (const auto& item : errors) {
    if (!text.empty()) {
      text += "; ";
    }
    text += item.message;
  }
  return text;
}

auto ColorGradeOf(const PipelineDocument& document, const NodeId& id, std::string* error)
    -> const ColorGradeNodeModel* {
  const auto* grade = dynamic_cast<const ColorGradeNodeModel*>(document.Graph().FindNode(id));
  if (grade == nullptr) {
    SetError(error, "Color Grade node is missing: " + std::string(id.Value()));
    return nullptr;
  }
  return grade;
}

auto IsDrtPostAdjustmentField(std::string_view field) -> bool {
  return field == "clarity" || field == "sharpen" || field == "halation" || field == "film_grain";
}

auto OperatorTypeForCurrentPanelField(std::string_view field) -> const OperatorTypeId* {
  if (field == "exposure") return &type_ids::Exposure();
  if (field == "contrast") return &type_ids::Contrast();
  if (field == "white" || field == "whites") return &type_ids::White();
  if (field == "black" || field == "blacks") return &type_ids::Black();
  if (field == "shadows") return &type_ids::Shadows();
  if (field == "highlights") return &type_ids::Highlights();
  if (field == "curve") return &type_ids::Curve();
  if (field == "saturation") return &type_ids::Saturation();
  if (field == "vibrance") return &type_ids::Vibrance();
  if (field == "grade_white_balance") return &type_ids::Cat02WhiteBalance();
  if (field == "hls" || field == "HLS") return &type_ids::Hls();
  if (field == "color_wheel") return &type_ids::ColorWheel();
  if (field == "lut" || field == "ocio_lmt") return &type_ids::Lmt();
  if (field == "clarity") return &type_ids::Clarity();
  if (field == "sharpen") return &type_ids::Sharpen();
  if (field == "halation") return &type_ids::Halation();
  if (field == "film_grain") return &type_ids::FilmGrain();
  return nullptr;
}

auto CurrentPanelColorGrade(const PipelineDocument& document) -> const ColorGradeNodeModel* {
  if (const auto* primary = document.PrimaryGrade(); primary != nullptr) {
    return primary;
  }
  const auto grades = ColorGradesOnImageBackbone(document);
  return grades.empty() ? nullptr : grades.front();
}

auto DrtPostModel(const DrtNodeModel& drt, const AdjustmentInstanceId& id, std::string* error)
    -> const IOperatorModel* {
  const auto* model = drt.FindAdjustment(id);
  if (model == nullptr) {
    SetError(error, "Adjustment instance is missing: " + std::string(id.Value()));
  }
  return model;
}

}  // namespace

auto ApplyEditorParameterPatch(PipelineDocument& document, const EditorParameterTarget& target,
                               const nlohmann::json& params, std::string* error) -> bool {
  auto write = ParseEditorParameterWrite(target.field_key, params, error);
  if (!write.has_value()) {
    return false;
  }
  return ApplyEditorParameterWrite(document, target, *write, error);
}

auto ApplyEditorParameterValue(PipelineDocument& document, const EditorParameterTarget& target,
                               const nlohmann::json& value, std::string* error) -> bool {
  auto write = ParseEditorParameterValue(target.field_key, value, error);
  if (!write.has_value()) {
    return false;
  }
  return ApplyEditorParameterWrite(document, target, *write, error);
}

auto ReadEditorParameterJson(const PipelineDocument& document, const EditorParameterTarget& target,
                             nlohmann::json* json, std::string* error) -> bool {
  if (json == nullptr) {
    return SetError(error, "Editor parameter JSON storage is required");
  }
  const auto target_error = DescribeEditorParameterTargetError(target, target.field_key);
  if (!target_error.empty()) {
    return SetError(error, target_error);
  }
  try {
    switch (target.owner_kind) {
      case EditorParameterOwnerKind::ColorGrade: {
        const auto* grade = ColorGradeOf(document, target.node_id, error);
        if (grade == nullptr) {
          return false;
        }
        const auto* model = grade->FindAdjustment(target.adjustment_instance_id);
        if (model == nullptr) {
          return SetError(error, "Adjustment instance is missing: " +
                                     std::string(target.adjustment_instance_id.Value()));
        }
        *json = model->ToJson();
        return true;
      }
      case EditorParameterOwnerKind::Document:
        *json = document.Geometry().ToJson();
        return true;
      case EditorParameterOwnerKind::Develop: {
        const auto* develop = document.Develop();
        if (develop == nullptr || develop->Id() != target.node_id) {
          return SetError(error, "Develop node is missing: " + std::string(target.node_id.Value()));
        }
        *json = develop->Params().ToJson();
        return true;
      }
      case EditorParameterOwnerKind::DrtPost: {
        const auto* drt = document.Drt();
        if (drt == nullptr || drt->Id() != target.node_id) {
          return SetError(error, "DRT node is missing: " + std::string(target.node_id.Value()));
        }
        if (IsDrtPostAdjustmentField(target.field_key)) {
          const auto* model = DrtPostModel(*drt, target.adjustment_instance_id, error);
          if (model == nullptr) {
            return false;
          }
          *json = model->ToJson();
          return true;
        }
        if (target.field_key == "diffusion") {
          *json = {{"strength", drt->Params().DiffusionStrength()}};
          return true;
        }
        if (target.field_key == "film_format") {
          *json = {{"film_format", std::string{FilmFormatId(drt->Params().FilmFormat())}}};
          return true;
        }
        *json = drt->Params().OutputTransformJson();
        return true;
      }
      default:
        return SetError(error, "Editor parameter target owner_kind is not supported");
    }
  } catch (const std::exception& ex) {
    return SetError(error, ex.what());
  }
}

auto FilmPrintEntrySaturationTarget(const PipelineDocument& document,
                                    const nlohmann::json&   odt_before,
                                    const nlohmann::json&   odt_after)
    -> std::optional<EditorParameterTarget> {
  const auto is_film_print = [](const nlohmann::json& odt) {
    return odt.value("method", std::string{}) == "film_print";
  };
  if (is_film_print(odt_before) || !is_film_print(odt_after)) {
    return std::nullopt;
  }
  const auto* grade = CurrentPanelColorGrade(document);
  if (grade == nullptr) {
    return std::nullopt;
  }
  const auto* saturation =
      dynamic_cast<const SaturationModel*>(grade->FindAdjustmentByType(type_ids::Saturation()));
  if (saturation == nullptr || saturation->Value() != kDefaultPipelineSaturation) {
    return std::nullopt;
  }
  std::string ignored;
  return CompleteCurrentPanelParameterTarget(document, "saturation", &ignored);
}

namespace {

/// True when the RGB curve of @p grade has the points of the Film-like preset of the RAW import
/// look.
auto HasImportLookCurve(const ColorGradeNodeModel& grade) -> bool {
  const auto* curve =
      dynamic_cast<const CurveModel*>(grade.FindAdjustmentByType(type_ids::Curve()));
  const auto* preset = FindToneCurvePreset(kDefaultPipelineToneCurvePresetId);
  if (curve == nullptr || preset == nullptr) return false;
  const auto points = curve->Points();
  return MatchToneCurvePreset(points) == preset;
}

/// The curve write of an entry reset: the RGB curve becomes the identity, the channel curves stay.
auto IdentityRgbCurvePatch() -> nlohmann::json {
  nlohmann::json points = nlohmann::json::array();
  for (const auto& point : IdentityCurvePoints()) {
    points.push_back({{"x", point.x}, {"y", point.y}});
  }
  return {{"curve", {{"points", std::move(points)}}}};
}

}  // namespace

auto FilmPrintEntryCurveTarget(const PipelineDocument& document, const nlohmann::json& odt_before,
                               const nlohmann::json& odt_after)
    -> std::optional<EditorParameterTarget> {
  const auto is_film_print = [](const nlohmann::json& odt) {
    return odt.value("method", std::string{}) == "film_print";
  };
  if (is_film_print(odt_before) || !is_film_print(odt_after)) {
    return std::nullopt;
  }
  const auto* grade = CurrentPanelColorGrade(document);
  if (grade == nullptr || !HasImportLookCurve(*grade)) {
    return std::nullopt;
  }
  std::string ignored;
  return CompleteCurrentPanelParameterTarget(document, "curve", &ignored);
}

namespace {

/// True when the DRT method of @p document runs the profile tone stage (study section 6.2).
auto MethodRunsProfileTone(const PipelineDocument& document) -> bool {
  const auto* drt = document.Drt();
  if (drt == nullptr) return false;
  const auto method = drt->Params().Method();
  return method == DrtMethod::Aces20 || method == DrtMethod::OpenDrt;
}

/// True when @p reference selects the stage and its bound profile has a curve.
auto ReferenceRunsProfileTone(const std::optional<DevelopCameraProfileFile>& reference) -> bool {
  return reference.has_value() && reference->profile_tone && reference->IsBound() &&
         reference->profile->profile_tone_curve != nullptr;
}

/// True when the reference of a Develop Model JSON selects the stage and the DCP library binds
/// its identity to a profile with a curve.
auto StoredReferenceRunsProfileTone(const nlohmann::json& develop) -> bool {
  const auto found = develop.find("camera_profile_file");
  if (found == develop.end() || found->is_null()) return false;
  const auto stored = DevelopCameraProfileFileFromJson(*found);
  return stored.profile_tone && ReferenceRunsProfileTone(ResolveStoredDcpCameraProfileFile(stored));
}

}  // namespace

auto ProfileToneEntryResetTargets(const PipelineDocument& document,
                                  const nlohmann::json&   develop_before)
    -> std::vector<EditorEntryResetWrite> {
  const auto* develop = document.Develop();
  if (develop == nullptr || !MethodRunsProfileTone(document) ||
      !ReferenceRunsProfileTone(develop->Params().CameraProfileFile())) {
    return {};
  }
  // The stage ran before the edit too (another profile with a curve): no entry.
  if (StoredReferenceRunsProfileTone(develop_before)) return {};
  const auto* grade = document.PrimaryGrade();
  if (grade == nullptr) return {};
  std::vector<EditorEntryResetWrite> writes;
  const auto                         add = [&](const char* field, float value) {
    std::string ignored;
    auto        target = CompleteCurrentPanelParameterTarget(document, field, &ignored);
    if (target.has_value()) {
      writes.push_back({std::move(*target), {{field, value}}});
    }
  };
  const auto* contrast =
      dynamic_cast<const ContrastModel*>(grade->FindAdjustmentByType(type_ids::Contrast()));
  if (contrast != nullptr && contrast->Value() == kImportLookContrastBeforeFilmLike) {
    add("contrast", kProfileToneEntryContrast);
  }
  const auto* saturation =
      dynamic_cast<const SaturationModel*>(grade->FindAdjustmentByType(type_ids::Saturation()));
  if (saturation != nullptr && saturation->Value() == kDefaultPipelineSaturation) {
    add("saturation", kProfileToneEntrySaturation);
  }
  if (HasImportLookCurve(*grade)) {
    std::string ignored;
    auto        target = CompleteCurrentPanelParameterTarget(document, "curve", &ignored);
    if (target.has_value()) {
      writes.push_back({std::move(*target), IdentityRgbCurvePatch()});
    }
  }
  return writes;
}

auto EntryResetWrites(const PipelineDocument& document, std::string_view field_key,
                      const nlohmann::json& before, const nlohmann::json& after)
    -> std::vector<EditorEntryResetWrite> {
  if (field_key == "odt") {
    std::vector<EditorEntryResetWrite> writes;
    if (auto target = FilmPrintEntrySaturationTarget(document, before, after)) {
      writes.push_back({std::move(*target), {{"saturation", kFilmPrintEntrySaturation}}});
    }
    if (auto target = FilmPrintEntryCurveTarget(document, before, after)) {
      writes.push_back({std::move(*target), IdentityRgbCurvePatch()});
    }
    return writes;
  }
  if (field_key == "camera_profile_file") {
    return ProfileToneEntryResetTargets(document, before);
  }
  return {};
}

auto CanonicalPipelineDocumentJson(const PipelineDocument& document) -> std::string {
  return document.ToJson().dump();
}

auto PipelineDocumentPassesValidation(const PipelineDocument& document, std::string* error)
    -> bool {
  auto       errors   = document.Graph().Validate();
  const auto backbone = document.Graph().ValidateImageBackbone();
  errors.insert(errors.end(), backbone.begin(), backbone.end());
  if (errors.empty()) {
    return true;
  }
  return SetError(error, JoinValidation(errors));
}

auto CompleteCurrentPanelParameterTarget(const PipelineDocument& document, std::string field_key,
                                         std::string* error)
    -> std::optional<EditorParameterTarget> {
  EditorParameterTarget target;
  target.field_key = std::move(field_key);
  if (target.field_key == "crop_rotate") {
    target.owner_kind = EditorParameterOwnerKind::Document;
    return target;
  }
  if (target.field_key == "raw_decode" || target.field_key == "lens_calib" ||
      target.field_key == "color_temp" || target.field_key == "input_profile" ||
      target.field_key == "camera_profile_file") {
    const auto* develop = document.Develop();
    if (develop == nullptr) {
      SetError(error, "Develop node is missing");
      return std::nullopt;
    }
    target.owner_kind = EditorParameterOwnerKind::Develop;
    target.node_id    = develop->Id();
    return target;
  }
  if (target.field_key == "odt" || target.field_key == "diffusion" ||
      target.field_key == "film_format") {
    const auto* drt = document.Drt();
    if (drt == nullptr) {
      SetError(error, "DRT node is missing");
      return std::nullopt;
    }
    target.owner_kind = EditorParameterOwnerKind::DrtPost;
    target.node_id    = drt->Id();
    return target;
  }
  const auto* type = OperatorTypeForCurrentPanelField(target.field_key);
  if (type == nullptr) {
    SetError(error, "Unknown editor adjustment field: " + target.field_key);
    return std::nullopt;
  }
  if (IsDrtPostAdjustmentField(target.field_key)) {
    const auto* drt = document.Drt();
    if (drt == nullptr) {
      SetError(error, "DRT node is missing");
      return std::nullopt;
    }
    const auto* instance = drt->FindAdjustmentIdByType(*type);
    if (instance == nullptr) {
      SetError(error, "DRT/Post adjustment is missing: " + std::string{type->Text()});
      return std::nullopt;
    }
    target.owner_kind             = EditorParameterOwnerKind::DrtPost;
    target.node_id                = drt->Id();
    target.adjustment_instance_id = *instance;
    return target;
  }
  const auto* grade = CurrentPanelColorGrade(document);
  if (grade == nullptr) {
    SetError(error, "Color Grade node is missing");
    return std::nullopt;
  }
  const auto* instance = grade->FindAdjustmentIdByType(*type);
  if (instance == nullptr) {
    SetError(error, "Color Grade adjustment is missing: " + std::string{type->Text()});
    return std::nullopt;
  }
  target.owner_kind             = EditorParameterOwnerKind::ColorGrade;
  target.node_id                = grade->Id();
  target.adjustment_instance_id = *instance;
  return target;
}

}  // namespace alcedo
