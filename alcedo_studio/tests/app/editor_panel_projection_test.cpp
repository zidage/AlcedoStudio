//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_panel_projection.hpp"

#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "app/editor_parameter_write.hpp"
#include "app/editor_pipeline_command_service.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/operators/models/cat02_white_balance_model.hpp"
#include "edit/operators/models/curve_model.hpp"
#include "edit/operators/models/dirty_field_mask.hpp"
#include "edit/operators/models/i_operator_model.hpp"
#include "edit/operators/models/lmt_model.hpp"
#include "edit/operators/models/operator_param_dto.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/operators/models/sharpen_model.hpp"
#include "json.hpp"
#include "support/editor_parameter_target_test.hpp"

namespace alcedo {
namespace {

class SerializationCountingModel : public IOperatorModel {
 public:
  mutable int json_reads = 0;
  mutable int dto_reads  = 0;
  int         loads      = 0;

  auto Type() const -> OperatorTypeId override { return value_.Type(); }
  auto IsDefault() const -> bool override { return value_.IsDefault(); }
  auto IsDirty() const -> bool override { return value_.IsDirty(); }
  auto MakeFullDto() const -> OperatorParamDto override {
    ++dto_reads;
    return value_.MakeFullDto();
  }
  auto TakeDirtyPatch() -> std::optional<OperatorParamPatchDto> override {
    return value_.TakeDirtyPatch();
  }
  void RestoreDirty(DirtyFieldMask fields) override { value_.RestoreDirty(fields); }
  void MarkAllDirty() override { value_.MarkAllDirty(); }
  auto ToJson() const -> nlohmann::json override {
    ++json_reads;
    return value_.ToJson();
  }
  void LoadJson(const nlohmann::json& json) override {
    ++loads;
    value_.LoadJson(json);
  }
  void SetValue(float value) { value_.SetValue(value); }
  [[nodiscard]] auto value() const -> float { return value_.Value(); }

 private:
  ExposureModel value_;
};

auto FindField(const EditorPanelProjection& projection, std::string_view key)
    -> const EditorPanelFieldPresentation* {
  for (const auto& field : projection.fields) {
    if (field.field_key == key) {
      return &field;
    }
  }
  return nullptr;
}

auto ScalarOf(const EditorPanelProjection& projection, std::string_view key) -> std::optional<float> {
  const auto* field = FindField(projection, key);
  if (field == nullptr) {
    return std::nullopt;
  }
  const auto* scalar = std::get_if<EditorPanelScalarValue>(&field->value);
  if (scalar == nullptr) {
    return std::nullopt;
  }
  return scalar->value;
}

auto NestedOf(const EditorPanelProjection& projection, std::string_view key)
    -> const EditorPanelNestedScalarValue* {
  const auto* field = FindField(projection, key);
  if (field == nullptr) {
    return nullptr;
  }
  return std::get_if<EditorPanelNestedScalarValue>(&field->value);
}

auto ReadDebugGain(const PipelineDocument& document, const EditorParameterTarget& target,
                   EditorPanelFieldPresentation* out, std::string* error) -> bool {
  const auto* grade =
      dynamic_cast<const ColorGradeNodeModel*>(document.Graph().FindNode(target.node_id));
  if (grade == nullptr) {
    if (error != nullptr) {
      *error = "Color Grade node is missing";
    }
    return false;
  }
  const auto* typed =
      dynamic_cast<const ExposureModel*>(grade->FindAdjustment(target.adjustment_instance_id));
  if (typed == nullptr) {
    if (error != nullptr) {
      *error = "debug_gain requires an Exposure Model";
    }
    return false;
  }
  out->field_key = target.field_key;
  out->source    = target;
  out->value     = EditorPanelScalarValue{"gain", typed->Value()};
  return true;
}

void SetOwnedScalars(PipelineDocument& document) {
  dynamic_cast<ExposureModel*>(document.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()))
      ->SetValue(2.25f);
  dynamic_cast<ContrastModel*>(document.PrimaryGrade()->FindAdjustmentByType(type_ids::Contrast()))
      ->SetValue(18.0f);
  dynamic_cast<SaturationModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(type_ids::Saturation()))
      ->SetValue(1.8f);
  dynamic_cast<CurveModel*>(document.PrimaryGrade()->FindAdjustmentByType(type_ids::Curve()))
      ->SetPoints({{0.0f, 0.0f}, {0.4f, 0.55f}, {1.0f, 1.0f}});
  dynamic_cast<LmtModel*>(document.PrimaryGrade()->FindAdjustmentByType(type_ids::Lmt()))
      ->SetCubePath("D:/luts/look.cube");
  dynamic_cast<Cat02WhiteBalanceModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(type_ids::Cat02WhiteBalance()))
      ->SetTintOffset(12.0f);
  dynamic_cast<ClarityModel*>(document.Drt()->FindAdjustmentByType(type_ids::Clarity()))
      ->SetValue(7.0f);
  dynamic_cast<FilmGrainModel*>(document.Drt()->FindAdjustmentByType(type_ids::FilmGrain()))
      ->SetValue(0.4f);
  dynamic_cast<SharpenModel*>(document.Drt()->FindAdjustmentByType(type_ids::Sharpen()))
      ->SetAmount(0.35f);

  DevelopRawDecodeUpdate raw;
  raw.demosaic_method       = std::string{"dht"};
  raw.highlights_reconstruct = false;
  document.Develop()->Params().ApplyRawDecodeUpdate(raw);

  DrtParameterUpdate odt;
  odt.encoding_space = DrtColorSpace::Rec2020;
  odt.encoding_eotf  = DrtEotf::St2084;
  odt.peak_luminance = 1600.0f;
  document.Drt()->Params().ApplyUpdate(odt);

  ImageGeometryUpdate geometry;
  geometry.crop_rect        = NormalizedRect{0.1f, 0.2f, 0.5f, 0.6f};
  geometry.rotation_degrees = 3.5f;
  document.Geometry().ApplyUpdate(geometry);

  DevelopLensCalibrationUpdate lens;
  lens.lens_enabled = true;
  document.Develop()->Params().ApplyLensCalibrationUpdate(lens);
}

}  // namespace

TEST(EditorPanelProjectionTest, ProjectsToneLookLutRawOdtAndGeometryFromExplicitInstances) {
  auto document = CreateDefaultPipelineDocument();
  SetOwnedScalars(document);

  EditorPanelProjection projection;
  std::string           error;
  ASSERT_TRUE(ProjectCurrentPanelFields(document, 41, &projection, &error)) << error;
  EXPECT_EQ(projection.session_generation, 41u);
  EXPECT_EQ(projection.fields.size(), EditorPanelAdapterTable::Production().Adapters().size());

  EXPECT_FLOAT_EQ(*ScalarOf(projection, "exposure"), 2.25f);
  EXPECT_FLOAT_EQ(*ScalarOf(projection, "contrast"), 18.0f);
  EXPECT_FLOAT_EQ(*ScalarOf(projection, "saturation"), 1.8f);
  EXPECT_FLOAT_EQ(*ScalarOf(projection, "tint"), 12.0f);
  EXPECT_FLOAT_EQ(*ScalarOf(projection, "clarity"), 7.0f);

  const auto* exposure = FindField(projection, "exposure");
  ASSERT_NE(exposure, nullptr);
  EXPECT_EQ(exposure->source.adjustment_instance_id.Value(), "grade.primary.exposure");
  EXPECT_EQ(exposure->source.node_id.Value(), "grade.primary");

  const auto* lut = FindField(projection, "lut");
  ASSERT_NE(lut, nullptr);
  EXPECT_EQ(lut->source.adjustment_instance_id.Value(), "grade.primary.lmt");
  const auto* lut_value = std::get_if<EditorPanelLutValue>(&lut->value);
  ASSERT_NE(lut_value, nullptr);
  EXPECT_EQ(lut_value->cube_path, "D:/luts/look.cube");

  const auto* curve = FindField(projection, "curve");
  ASSERT_NE(curve, nullptr);
  const auto* curve_value = std::get_if<EditorPanelCurveValue>(&curve->value);
  ASSERT_NE(curve_value, nullptr);
  ASSERT_EQ(curve_value->points.size(), 3u);
  EXPECT_FLOAT_EQ(curve_value->points[1].x, 0.4f);
  EXPECT_FLOAT_EQ(curve_value->points[1].y, 0.55f);

  const auto* grain = NestedOf(projection, "film_grain");
  ASSERT_NE(grain, nullptr);
  EXPECT_EQ(grain->object_key, "film_grain");
  EXPECT_EQ(grain->value_key, "strength");
  EXPECT_FLOAT_EQ(grain->value, 0.4f);

  const auto* sharpen = NestedOf(projection, "sharpen");
  ASSERT_NE(sharpen, nullptr);
  EXPECT_EQ(sharpen->value_key, "offset");
  EXPECT_FLOAT_EQ(sharpen->value, 0.35f);

  const auto* raw = FindField(projection, "raw_decode");
  ASSERT_NE(raw, nullptr);
  const auto* raw_value = std::get_if<EditorPanelRawDecodeValue>(&raw->value);
  ASSERT_NE(raw_value, nullptr);
  EXPECT_EQ(raw_value->method, "dht");
  EXPECT_FALSE(raw_value->highlights_reconstruct);

  const auto* odt = FindField(projection, "odt");
  ASSERT_NE(odt, nullptr);
  const auto* odt_value = std::get_if<EditorPanelOdtValue>(&odt->value);
  ASSERT_NE(odt_value, nullptr);
  EXPECT_EQ(odt_value->encoding_space, "rec2020");
  EXPECT_EQ(odt_value->encoding_eotf, "st2084");
  EXPECT_FLOAT_EQ(odt_value->peak_luminance, 1600.0f);

  const auto* geometry = FindField(projection, "crop_rotate");
  ASSERT_NE(geometry, nullptr);
  const auto* geometry_value = std::get_if<EditorPanelGeometryValue>(&geometry->value);
  ASSERT_NE(geometry_value, nullptr);
  EXPECT_FLOAT_EQ(geometry_value->crop_rect.x, 0.1f);
  EXPECT_FLOAT_EQ(geometry_value->rotation_degrees, 3.5f);

  const auto* lens = FindField(projection, "lens_calib");
  ASSERT_NE(lens, nullptr);
  const auto* lens_value = std::get_if<EditorPanelLensValue>(&lens->value);
  ASSERT_NE(lens_value, nullptr);
  EXPECT_TRUE(lens_value->enabled);
}

TEST(EditorPanelProjectionTest, ReadsNamedInstanceNotFirstOperatorOfType) {
  auto document = CreateDefaultPipelineDocument();
  auto* primary = dynamic_cast<ExposureModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(primary, nullptr);
  primary->SetValue(1.25f);

  auto second = std::make_unique<ExposureModel>();
  second->SetValue(4.5f);
  document.InsertAdjustment(NodeId{"grade.primary"}, document.PrimaryGrade()->AdjustmentCount(),
                            AdjustmentInstanceId{"grade.primary.exposure.b"}, std::move(second));

  auto first_by_type = CompleteCurrentPanelParameterTarget(document, "exposure", nullptr);
  ASSERT_TRUE(first_by_type.has_value());
  EXPECT_EQ(first_by_type->adjustment_instance_id.Value(), "grade.primary.exposure");

  auto named = test::ColorGradeFieldTarget("exposure");
  named.adjustment_instance_id = AdjustmentInstanceId{"grade.primary.exposure.b"};

  EditorPanelFieldPresentation field;
  std::string                  error;
  ASSERT_TRUE(ReadEditorPanelField(document, named, &field, &error)) << error;
  const auto* scalar = std::get_if<EditorPanelScalarValue>(&field.value);
  ASSERT_NE(scalar, nullptr);
  EXPECT_FLOAT_EQ(scalar->value, 4.5f);
  EXPECT_EQ(field.source.adjustment_instance_id.Value(), "grade.primary.exposure.b");

  EditorPanelFieldPresentation current;
  ASSERT_TRUE(ReadEditorPanelField(document, *first_by_type, &current, &error)) << error;
  const auto* current_scalar = std::get_if<EditorPanelScalarValue>(&current.value);
  ASSERT_NE(current_scalar, nullptr);
  EXPECT_FLOAT_EQ(current_scalar->value, 1.25f);
}

TEST(EditorPanelProjectionTest, MissingNodeOrInstanceFailsBeforeAnyPanelValue) {
  auto document = CreateDefaultPipelineDocument();
  auto* exposure = dynamic_cast<ExposureModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(2.0f);

  EditorPanelProjection sentinel;
  sentinel.session_generation = 99;
  EditorPanelFieldPresentation leftover;
  leftover.field_key = "leftover";
  leftover.value     = EditorPanelScalarValue{"leftover", 7.0f};
  sentinel.fields.push_back(leftover);

  auto missing_node = test::ColorGradeFieldTarget("exposure");
  missing_node.node_id = NodeId{"grade.absent"};
  auto missing_instance = test::ColorGradeFieldTarget("exposure");
  missing_instance.adjustment_instance_id = AdjustmentInstanceId{"grade.primary.missing"};
  const auto good = test::ColorGradeFieldTarget("exposure");

  std::string error;
  EXPECT_FALSE(ProjectEditorPanelFields(document, std::array{good, missing_node}, 1, &sentinel,
                                        &error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(sentinel.session_generation, 99u);
  ASSERT_EQ(sentinel.fields.size(), 1u);
  EXPECT_EQ(sentinel.fields.front().field_key, "leftover");

  error.clear();
  EXPECT_FALSE(ProjectEditorPanelFields(document, std::array{good, missing_instance}, 1, &sentinel,
                                        &error));
  EXPECT_EQ(sentinel.session_generation, 99u);
  EXPECT_EQ(sentinel.fields.front().field_key, "leftover");
}

TEST(EditorPanelProjectionTest, PanelProjectionDoesNotCallModelJsonOrFullDto) {
  auto document = CreateDefaultPipelineDocument();
  auto counted  = std::make_unique<SerializationCountingModel>();
  counted->SetValue(3.0f);
  auto* observed = counted.get();
  document.InsertAdjustment(NodeId{"grade.primary"}, document.PrimaryGrade()->AdjustmentCount(),
                            AdjustmentInstanceId{"counted"}, std::move(counted));

  auto counted_target                   = test::ColorGradeFieldTarget("exposure");
  counted_target.adjustment_instance_id = AdjustmentInstanceId{"counted"};

  EditorPanelFieldPresentation field;
  std::string                  error;
  EXPECT_FALSE(ReadEditorPanelField(document, counted_target, &field, &error));
  EXPECT_EQ(observed->json_reads, 0);
  EXPECT_EQ(observed->dto_reads, 0);
  EXPECT_EQ(observed->loads, 0);

  EditorPanelProjection projection;
  ASSERT_TRUE(ProjectCurrentPanelFields(document, 2, &projection, &error)) << error;
  EXPECT_FLOAT_EQ(*ScalarOf(projection, "exposure"), kDefaultPipelineExposureEv);
  EXPECT_EQ(observed->json_reads, 0);
  EXPECT_EQ(observed->dto_reads, 0);

  nlohmann::json json;
  ASSERT_TRUE(ReadEditorParameterJson(document, counted_target, &json, &error)) << error;
  EXPECT_EQ(observed->json_reads, 1);
  EXPECT_EQ(observed->dto_reads, 0);

  const auto reads_after_json_read = observed->json_reads;
  const auto persisted             = CanonicalPipelineDocumentJson(document);
  EXPECT_FALSE(persisted.empty());
  EXPECT_GT(observed->json_reads, reads_after_json_read);
}

TEST(EditorPanelProjectionTest, AdditionalPanelAdapterDoesNotChangeParameterWriteParser) {
  auto document = CreateDefaultPipelineDocument();
  auto* exposure = dynamic_cast<ExposureModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(1.75f);

  auto table = EditorPanelAdapterTable::Production();
  table.Add({"debug_gain", "tone", &ReadDebugGain});

  auto target = test::ColorGradeFieldTarget("exposure");
  target.field_key = "debug_gain";

  EditorPanelProjection projection;
  std::string           error;
  ASSERT_TRUE(ProjectEditorPanelFields(document, std::array{target}, 3, table, &projection, &error))
      << error;
  ASSERT_EQ(projection.fields.size(), 1u);
  EXPECT_EQ(projection.fields.front().field_key, "debug_gain");
  const auto* scalar = std::get_if<EditorPanelScalarValue>(&projection.fields.front().value);
  ASSERT_NE(scalar, nullptr);
  EXPECT_FLOAT_EQ(scalar->value, 1.75f);

  std::string parse_error;
  EXPECT_FALSE(ParseEditorParameterWrite("debug_gain", nlohmann::json{{"gain", 1.0}}, &parse_error)
                   .has_value());
  EXPECT_FALSE(parse_error.empty());
  parse_error.clear();
  EXPECT_TRUE(ParseEditorParameterWrite("exposure", nlohmann::json{{"exposure", 0.5}}, &parse_error)
                  .has_value())
      << parse_error;
}

}  // namespace alcedo
