//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_panel_projection.hpp"

#include <algorithm>
#include <utility>

#include "app/editor_pipeline_command_service.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/operators/models/cat02_white_balance_model.hpp"
#include "edit/operators/models/lmt_model.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/operators/models/sharpen_model.hpp"

namespace alcedo {
namespace {

auto SetError(std::string* error, std::string message) -> bool {
  if (error != nullptr) {
    *error = std::move(message);
  }
  return false;
}

auto FinishField(const EditorParameterTarget& target, EditorPanelFieldValue value,
                 EditorPanelFieldPresentation* out) -> bool {
  out->field_key = target.field_key;
  out->source    = target;
  out->value     = std::move(value);
  return true;
}

auto ColorGradeModel(const PipelineDocument& document, const EditorParameterTarget& target,
                     std::string* error) -> const IOperatorModel* {
  const auto* grade =
      dynamic_cast<const ColorGradeNodeModel*>(document.Graph().FindNode(target.node_id));
  if (grade == nullptr) {
    SetError(error, "Color Grade node is missing: " + std::string(target.node_id.Value()));
    return nullptr;
  }
  const auto* model = grade->FindAdjustment(target.adjustment_instance_id);
  if (model == nullptr) {
    SetError(error, "Adjustment instance is missing: " +
                        std::string(target.adjustment_instance_id.Value()));
    return nullptr;
  }
  return model;
}

auto DrtAdjustment(const PipelineDocument& document, const EditorParameterTarget& target,
                   std::string* error) -> const IOperatorModel* {
  if (target.owner_kind != EditorParameterOwnerKind::DrtPost) {
    SetError(error, "DRT/Post panel field requires a DRT target");
    return nullptr;
  }
  const auto* drt = document.Drt();
  if (drt == nullptr || drt->Id() != target.node_id) {
    SetError(error, "DRT node is missing: " + std::string(target.node_id.Value()));
    return nullptr;
  }
  const auto* model = drt->FindAdjustment(target.adjustment_instance_id);
  if (model == nullptr) {
    SetError(error, "Adjustment instance is missing: " +
                        std::string(target.adjustment_instance_id.Value()));
    return nullptr;
  }
  return model;
}

auto DevelopParams(const PipelineDocument& document, const EditorParameterTarget& target,
                   std::string* error) -> const DevelopParamsModel* {
  if (target.owner_kind != EditorParameterOwnerKind::Develop) {
    SetError(error, "Develop panel field requires a Develop target");
    return nullptr;
  }
  const auto* develop = document.Develop();
  if (develop == nullptr || develop->Id() != target.node_id) {
    SetError(error, "Develop node is missing: " + std::string(target.node_id.Value()));
    return nullptr;
  }
  return &develop->Params();
}

template <class Model>
auto ReadGradeScalar(const PipelineDocument& document, const EditorParameterTarget& target,
                     std::string_view display_key, EditorPanelFieldPresentation* out,
                     std::string* error) -> bool {
  const auto* model = ColorGradeModel(document, target, error);
  if (model == nullptr) {
    return false;
  }
  const auto* typed = dynamic_cast<const Model*>(model);
  if (typed == nullptr) {
    return SetError(error, "Adjustment Model type does not match field " + target.field_key);
  }
  return FinishField(target, EditorPanelScalarValue{std::string{display_key}, typed->Value()}, out);
}

template <class Model>
auto ReadDrtNestedScalar(const PipelineDocument& document, const EditorParameterTarget& target,
                         std::string_view object_key, std::string_view value_key,
                         EditorPanelFieldPresentation* out, std::string* error) -> bool {
  const auto* model = DrtAdjustment(document, target, error);
  if (model == nullptr) {
    return false;
  }
  const auto* typed = dynamic_cast<const Model*>(model);
  if (typed == nullptr) {
    return SetError(error, "Adjustment Model type does not match field " + target.field_key);
  }
  EditorPanelNestedScalarValue nested;
  nested.object_key = std::string{object_key};
  nested.value_key  = std::string{value_key};
  nested.value      = typed->Value();
  return FinishField(target, std::move(nested), out);
}

template <class Model>
auto ReadDrtFlatScalar(const PipelineDocument& document, const EditorParameterTarget& target,
                       std::string_view display_key, EditorPanelFieldPresentation* out,
                       std::string* error) -> bool {
  const auto* model = DrtAdjustment(document, target, error);
  if (model == nullptr) {
    return false;
  }
  const auto* typed = dynamic_cast<const Model*>(model);
  if (typed == nullptr) {
    return SetError(error, "Adjustment Model type does not match field " + target.field_key);
  }
  return FinishField(target, EditorPanelScalarValue{std::string{display_key}, typed->Value()}, out);
}

auto DrtMethodText(DrtMethod method) -> const char* {
  return method == DrtMethod::Aces20 ? "aces_2_0" : "open_drt";
}

auto DrtSpaceText(DrtColorSpace space) -> const char* {
  switch (space) {
    case DrtColorSpace::Rec2020:
      return "rec2020";
    case DrtColorSpace::P3D65:
      return "p3_d65";
    case DrtColorSpace::Rec709:
    default:
      return "rec709";
  }
}

auto DrtEotfText(DrtEotf eotf) -> const char* {
  switch (eotf) {
    case DrtEotf::Linear:
      return "linear";
    case DrtEotf::St2084:
      return "st2084";
    case DrtEotf::Hlg:
      return "hlg";
    case DrtEotf::Gamma26:
      return "gamma_2_6";
    case DrtEotf::Bt1886:
      return "bt1886";
    case DrtEotf::Gamma18:
      return "gamma_1_8";
    case DrtEotf::Gamma22:
    default:
      return "gamma_2_2";
  }
}

auto ReadExposure(const PipelineDocument& document, const EditorParameterTarget& target,
                  EditorPanelFieldPresentation* out, std::string* error) -> bool {
  return ReadGradeScalar<ExposureModel>(document, target, "exposure", out, error);
}

auto ReadContrast(const PipelineDocument& document, const EditorParameterTarget& target,
                  EditorPanelFieldPresentation* out, std::string* error) -> bool {
  return ReadGradeScalar<ContrastModel>(document, target, "contrast", out, error);
}

auto ReadWhite(const PipelineDocument& document, const EditorParameterTarget& target,
               EditorPanelFieldPresentation* out, std::string* error) -> bool {
  return ReadGradeScalar<WhiteModel>(document, target, "white", out, error);
}

auto ReadBlack(const PipelineDocument& document, const EditorParameterTarget& target,
               EditorPanelFieldPresentation* out, std::string* error) -> bool {
  return ReadGradeScalar<BlackModel>(document, target, "black", out, error);
}

auto ReadShadows(const PipelineDocument& document, const EditorParameterTarget& target,
                 EditorPanelFieldPresentation* out, std::string* error) -> bool {
  return ReadGradeScalar<ShadowsModel>(document, target, "shadows", out, error);
}

auto ReadHighlights(const PipelineDocument& document, const EditorParameterTarget& target,
                    EditorPanelFieldPresentation* out, std::string* error) -> bool {
  return ReadGradeScalar<HighlightsModel>(document, target, "highlights", out, error);
}

auto ReadSaturation(const PipelineDocument& document, const EditorParameterTarget& target,
                    EditorPanelFieldPresentation* out, std::string* error) -> bool {
  return ReadGradeScalar<SaturationModel>(document, target, "saturation", out, error);
}

auto ReadVibrance(const PipelineDocument& document, const EditorParameterTarget& target,
                  EditorPanelFieldPresentation* out, std::string* error) -> bool {
  return ReadGradeScalar<VibranceModel>(document, target, "vibrance", out, error);
}

auto ReadCurve(const PipelineDocument& document, const EditorParameterTarget& target,
               EditorPanelFieldPresentation* out, std::string* error) -> bool {
  const auto* model = ColorGradeModel(document, target, error);
  if (model == nullptr) {
    return false;
  }
  const auto* typed = dynamic_cast<const CurveModel*>(model);
  if (typed == nullptr) {
    return SetError(error, "Adjustment Model type does not match field curve");
  }
  return FinishField(target, EditorPanelCurveValue{typed->Points()}, out);
}

auto ReadLut(const PipelineDocument& document, const EditorParameterTarget& target,
             EditorPanelFieldPresentation* out, std::string* error) -> bool {
  const auto* model = ColorGradeModel(document, target, error);
  if (model == nullptr) {
    return false;
  }
  const auto* typed = dynamic_cast<const LmtModel*>(model);
  if (typed == nullptr) {
    return SetError(error, "Adjustment Model type does not match field lut");
  }
  return FinishField(target, EditorPanelLutValue{typed->CubePath()}, out);
}

auto ReadHls(const PipelineDocument& document, const EditorParameterTarget& target,
             EditorPanelFieldPresentation* out, std::string* error) -> bool {
  const auto* model = ColorGradeModel(document, target, error);
  if (model == nullptr) {
    return false;
  }
  const auto* typed = dynamic_cast<const HlsModel*>(model);
  if (typed == nullptr) {
    return SetError(error, "Adjustment Model type does not match field hls");
  }
  EditorPanelHlsValue value;
  value.hls_adj_table = typed->AdjustmentTable();
  value.h_range_table = typed->HueRangeTable();
  value.target_hls    = typed->TargetHls();
  return FinishField(target, std::move(value), out);
}

auto ReadColorWheel(const PipelineDocument& document, const EditorParameterTarget& target,
                    EditorPanelFieldPresentation* out, std::string* error) -> bool {
  const auto* model = ColorGradeModel(document, target, error);
  if (model == nullptr) {
    return false;
  }
  const auto* typed = dynamic_cast<const ColorWheelModel*>(model);
  if (typed == nullptr) {
    return SetError(error, "Adjustment Model type does not match field color_wheel");
  }
  EditorPanelColorWheelValue value;
  value.lift  = typed->Lift();
  value.gamma = typed->Gamma();
  value.gain  = typed->Gain();
  return FinishField(target, std::move(value), out);
}

auto ReadTint(const PipelineDocument& document, const EditorParameterTarget& target,
              EditorPanelFieldPresentation* out, std::string* error) -> bool {
  const auto* model = ColorGradeModel(document, target, error);
  if (model == nullptr) {
    return false;
  }
  const auto* typed = dynamic_cast<const Cat02WhiteBalanceModel*>(model);
  if (typed == nullptr) {
    return SetError(error, "Adjustment Model type does not match field tint");
  }
  return FinishField(target, EditorPanelScalarValue{"tint", typed->TintOffset()}, out);
}

auto ReadClarity(const PipelineDocument& document, const EditorParameterTarget& target,
                 EditorPanelFieldPresentation* out, std::string* error) -> bool {
  return ReadDrtFlatScalar<ClarityModel>(document, target, "clarity", out, error);
}

auto ReadHalation(const PipelineDocument& document, const EditorParameterTarget& target,
                  EditorPanelFieldPresentation* out, std::string* error) -> bool {
  return ReadDrtNestedScalar<HalationModel>(document, target, "halation", "strength", out, error);
}

auto ReadFilmGrain(const PipelineDocument& document, const EditorParameterTarget& target,
                   EditorPanelFieldPresentation* out, std::string* error) -> bool {
  return ReadDrtNestedScalar<FilmGrainModel>(document, target, "film_grain", "strength", out, error);
}

auto ReadSharpen(const PipelineDocument& document, const EditorParameterTarget& target,
                 EditorPanelFieldPresentation* out, std::string* error) -> bool {
  const auto* model = DrtAdjustment(document, target, error);
  if (model == nullptr) {
    return false;
  }
  const auto* typed = dynamic_cast<const SharpenModel*>(model);
  if (typed == nullptr) {
    return SetError(error, "Adjustment Model type does not match field sharpen");
  }
  EditorPanelNestedScalarValue nested;
  nested.object_key = "sharpen";
  nested.value_key  = "offset";
  nested.value      = typed->Amount();
  return FinishField(target, std::move(nested), out);
}

auto ReadOdt(const PipelineDocument& document, const EditorParameterTarget& target,
             EditorPanelFieldPresentation* out, std::string* error) -> bool {
  if (target.owner_kind != EditorParameterOwnerKind::DrtPost) {
    return SetError(error, "ODT panel field requires a DRT target");
  }
  const auto* drt = document.Drt();
  if (drt == nullptr || drt->Id() != target.node_id) {
    return SetError(error, "DRT node is missing: " + std::string(target.node_id.Value()));
  }
  const auto& params = drt->Params();
  EditorPanelOdtValue value;
  value.method           = DrtMethodText(params.Method());
  value.encoding_space   = DrtSpaceText(params.EncodingSpace());
  value.encoding_eotf    = DrtEotfText(params.EncodingEotf());
  value.limiting_space   = DrtSpaceText(params.LimitingSpace());
  value.peak_luminance   = params.PeakLuminance();
  value.look_preset      = params.LookPreset();
  value.tonescale_preset = params.TonescalePreset();
  value.creative_white   = params.CreativeWhite();
  return FinishField(target, std::move(value), out);
}

auto ReadRawDecode(const PipelineDocument& document, const EditorParameterTarget& target,
                   EditorPanelFieldPresentation* out, std::string* error) -> bool {
  const auto* params = DevelopParams(document, target, error);
  if (params == nullptr) {
    return false;
  }
  EditorPanelRawDecodeValue value;
  value.method                 = params->DemosaicMethod();
  value.highlights_reconstruct = params->HighlightsReconstruct();
  return FinishField(target, std::move(value), out);
}

auto ReadColorTemp(const PipelineDocument& document, const EditorParameterTarget& target,
                   EditorPanelFieldPresentation* out, std::string* error) -> bool {
  const auto* params = DevelopParams(document, target, error);
  if (params == nullptr) {
    return false;
  }
  EditorPanelColorTempValue value;
  value.mode         = params->WhiteBalanceMode();
  value.custom_cct   = params->CustomCct();
  value.custom_tint  = params->CustomTint();
  value.as_shot_cct  = params->AsShotCct();
  value.as_shot_tint = params->AsShotTint();
  return FinishField(target, std::move(value), out);
}

auto ReadLens(const PipelineDocument& document, const EditorParameterTarget& target,
              EditorPanelFieldPresentation* out, std::string* error) -> bool {
  const auto* params = DevelopParams(document, target, error);
  if (params == nullptr) {
    return false;
  }
  return FinishField(target, EditorPanelLensValue{params->LensEnabled()}, out);
}

auto ReadGeometry(const PipelineDocument& document, const EditorParameterTarget& target,
                  EditorPanelFieldPresentation* out, std::string* error) -> bool {
  if (target.owner_kind != EditorParameterOwnerKind::Document) {
    return SetError(error, "Geometry panel field requires a document target");
  }
  const auto& geometry = document.Geometry();
  EditorPanelGeometryValue value;
  value.crop_rect         = geometry.CropRect();
  value.rotation_degrees  = geometry.RotationDegrees();
  value.expand_to_fit     = geometry.ExpandToFit();
  return FinishField(target, std::move(value), out);
}

auto ReadThroughTable(const PipelineDocument& document, const EditorParameterTarget& target,
                      const EditorPanelAdapterTable& table, EditorPanelFieldPresentation* out,
                      std::string* error) -> bool {
  if (out == nullptr) {
    return SetError(error, "Panel field output is null");
  }
  const auto target_error = DescribeEditorParameterTargetError(target, target.field_key);
  if (!target_error.empty()) {
    return SetError(error, target_error);
  }
  const auto* adapter = table.Find(target.field_key);
  if (adapter == nullptr || adapter->read == nullptr) {
    return SetError(error, "No panel adapter for field: " + target.field_key);
  }
  EditorPanelFieldPresentation local;
  if (!adapter->read(document, target, &local, error)) {
    return false;
  }
  *out = std::move(local);
  return true;
}

auto ProjectThroughTable(const PipelineDocument& document,
                         std::span<const EditorParameterTarget> targets,
                         std::uint64_t session_generation, const EditorPanelAdapterTable& table,
                         EditorPanelProjection* out, std::string* error) -> bool {
  if (out == nullptr) {
    return SetError(error, "Panel projection output is null");
  }
  EditorPanelProjection local;
  local.session_generation = session_generation;
  local.fields.reserve(targets.size());
  for (const auto& target : targets) {
    EditorPanelFieldPresentation field;
    if (!ReadThroughTable(document, target, table, &field, error)) {
      return false;
    }
    local.fields.push_back(std::move(field));
  }
  *out = std::move(local);
  return true;
}

}  // namespace

auto EditorPanelAdapterTable::Production() -> EditorPanelAdapterTable {
  static const auto kProduction = [] {
    EditorPanelAdapterTable table;
    table.Add({"exposure", "tone", &ReadExposure});
    table.Add({"contrast", "tone", &ReadContrast});
    table.Add({"white", "tone", &ReadWhite});
    table.Add({"black", "tone", &ReadBlack});
    table.Add({"shadows", "tone", &ReadShadows});
    table.Add({"highlights", "tone", &ReadHighlights});
    table.Add({"curve", "tone", &ReadCurve});
    table.Add({"saturation", "look", &ReadSaturation});
    table.Add({"vibrance", "look", &ReadVibrance});
    table.Add({"tint", "look", &ReadTint});
    table.Add({"hls", "look", &ReadHls});
    table.Add({"color_wheel", "look", &ReadColorWheel});
    table.Add({"lut", "lut", &ReadLut});
    table.Add({"clarity", "look", &ReadClarity});
    table.Add({"sharpen", "look", &ReadSharpen});
    table.Add({"film_grain", "look", &ReadFilmGrain});
    table.Add({"halation", "look", &ReadHalation});
    table.Add({"odt", "display", &ReadOdt});
    table.Add({"raw_decode", "raw", &ReadRawDecode});
    table.Add({"color_temp", "look", &ReadColorTemp});
    table.Add({"lens_calib", "geometry", &ReadLens});
    table.Add({"crop_rotate", "geometry", &ReadGeometry});
    return table;
  }();
  return kProduction;
}

void EditorPanelAdapterTable::Add(EditorPanelAdapter adapter) { adapters_.push_back(adapter); }

auto EditorPanelAdapterTable::Find(std::string_view field_key) const -> const EditorPanelAdapter* {
  const auto found = std::find_if(adapters_.begin(), adapters_.end(),
                                  [field_key](const EditorPanelAdapter& adapter) {
                                    return adapter.field_key == field_key;
                                  });
  if (found == adapters_.end()) {
    return nullptr;
  }
  return &*found;
}

auto EditorPanelAdapterTable::Adapters() const -> std::span<const EditorPanelAdapter> {
  return adapters_;
}

auto EditorPanelProjectionIsCurrent(const EditorPanelProjection& projection,
                                    std::uint64_t session_generation) -> bool {
  return projection.session_generation == session_generation;
}

void UpsertEditorPanelField(EditorPanelProjection* projection, EditorPanelFieldPresentation field) {
  if (projection == nullptr) {
    return;
  }
  auto existing = std::find_if(projection->fields.begin(), projection->fields.end(),
                               [&](const EditorPanelFieldPresentation& current) {
                                 return current.field_key == field.field_key;
                               });
  if (existing == projection->fields.end()) {
    projection->fields.push_back(std::move(field));
    return;
  }
  *existing = std::move(field);
}

auto ReadEditorPanelField(const PipelineDocument& document, const EditorParameterTarget& target,
                          EditorPanelFieldPresentation* out, std::string* error) -> bool {
  const auto table = EditorPanelAdapterTable::Production();
  return ReadThroughTable(document, target, table, out, error);
}

auto ReadEditorPanelField(const PipelineDocument& document, const EditorParameterTarget& target,
                          const EditorPanelAdapterTable& table, EditorPanelFieldPresentation* out,
                          std::string* error) -> bool {
  return ReadThroughTable(document, target, table, out, error);
}

auto CurrentPanelProjectionTargets(const PipelineDocument& document, std::string* error)
    -> std::optional<std::vector<EditorParameterTarget>> {
  std::vector<EditorParameterTarget> targets;
  const auto                         table = EditorPanelAdapterTable::Production();
  targets.reserve(table.Adapters().size());
  for (const auto& adapter : table.Adapters()) {
    std::string field_error;
    auto        target =
        CompleteCurrentPanelParameterTarget(document, std::string{adapter.field_key}, &field_error);
    if (!target.has_value()) {
      SetError(error, field_error);
      return std::nullopt;
    }
    targets.push_back(*target);
  }
  return targets;
}

auto ProjectEditorPanelFields(const PipelineDocument& document,
                              std::span<const EditorParameterTarget> targets,
                              std::uint64_t session_generation, EditorPanelProjection* out,
                              std::string* error) -> bool {
  const auto table = EditorPanelAdapterTable::Production();
  return ProjectThroughTable(document, targets, session_generation, table, out, error);
}

auto ProjectEditorPanelFields(const PipelineDocument& document,
                              std::span<const EditorParameterTarget> targets,
                              std::uint64_t session_generation, const EditorPanelAdapterTable& table,
                              EditorPanelProjection* out, std::string* error) -> bool {
  return ProjectThroughTable(document, targets, session_generation, table, out, error);
}

auto ProjectCurrentPanelFields(const PipelineDocument& document, std::uint64_t session_generation,
                               EditorPanelProjection* out, std::string* error) -> bool {
  const auto targets = CurrentPanelProjectionTargets(document, error);
  if (!targets.has_value()) {
    return false;
  }
  return ProjectEditorPanelFields(document, *targets, session_generation, out, error);
}

}  // namespace alcedo
