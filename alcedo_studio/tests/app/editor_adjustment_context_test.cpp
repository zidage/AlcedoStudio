//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_adjustment_context.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_commands.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/dirty_field_mask.hpp"
#include "edit/operators/models/i_operator_model.hpp"
#include "edit/operators/models/operator_param_dto.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "image/image.hpp"
#include "image/metadata.hpp"

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
  auto DirtyFields() const -> DirtyFieldMask override { return value_.DirtyFields(); }
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

 private:
  ExposureModel value_;
};

auto ExposureOf(ColorGradeNodeModel* grade) -> ExposureModel* {
  return dynamic_cast<ExposureModel*>(grade->FindAdjustmentByType(type_ids::Exposure()));
}

auto ScalarOf(const EditorPanelProjection& projection, std::string_view key)
    -> std::optional<float> {
  for (const auto& field : projection.fields) {
    if (field.field_key != key) {
      continue;
    }
    const auto* scalar = std::get_if<EditorPanelScalarValue>(&field.value);
    if (scalar == nullptr) {
      return std::nullopt;
    }
    return scalar->value;
  }
  return std::nullopt;
}

}  // namespace

TEST(EditorAdjustmentContextTest, TwoColorGradesResolveIndependentExposureInstances) {
  auto document = CreateDefaultPipelineDocument();
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.b"}).empty());
  auto* extra = dynamic_cast<ColorGradeNodeModel*>(document.Graph().FindNode(NodeId{"grade.b"}));
  ASSERT_NE(extra, nullptr);
  auto* extra_exposure = ExposureOf(extra);
  ASSERT_NE(extra_exposure, nullptr);
  extra_exposure->SetValue(0.25f);

  std::string error;
  const auto  primary = CompleteSelectedNodeParameterTarget(document, NodeId{"grade.primary"},
                                                           "exposure", &error);
  const auto  second =
      CompleteSelectedNodeParameterTarget(document, NodeId{"grade.b"}, "exposure", &error);
  ASSERT_TRUE(primary.has_value()) << error;
  ASSERT_TRUE(second.has_value()) << error;
  EXPECT_EQ(primary->owner_kind, EditorParameterOwnerKind::ColorGrade);
  EXPECT_EQ(primary->node_id, NodeId{"grade.primary"});
  EXPECT_EQ(second->node_id, NodeId{"grade.b"});
  EXPECT_NE(primary->adjustment_instance_id, second->adjustment_instance_id);
  EXPECT_NE(second->adjustment_instance_id, AdjustmentInstanceId{"grade.primary.exposure"});

  EditorPanelProjection primary_fields;
  EditorPanelProjection extra_fields;
  ASSERT_TRUE(ProjectSelectedNodePanelFields(document, NodeId{"grade.primary"}, 1, &primary_fields,
                                             &error))
      << error;
  ASSERT_TRUE(
      ProjectSelectedNodePanelFields(document, NodeId{"grade.b"}, 1, &extra_fields, &error))
      << error;
  ASSERT_TRUE(ScalarOf(primary_fields, "exposure").has_value());
  ASSERT_TRUE(ScalarOf(extra_fields, "exposure").has_value());
  EXPECT_FLOAT_EQ(*ScalarOf(primary_fields, "exposure"), kDefaultPipelineExposureEv);
  EXPECT_FLOAT_EQ(*ScalarOf(extra_fields, "exposure"), 0.25f);
}

TEST(EditorAdjustmentContextTest, MissingNodeOrInstanceLeavesProjectionUnchanged) {
  auto document = CreateDefaultPipelineDocument();
  EditorPanelProjection sentinel;
  sentinel.session_generation = 99;
  EditorPanelFieldPresentation leftover;
  leftover.field_key = "leftover";
  sentinel.fields.push_back(leftover);

  std::string error;
  EXPECT_FALSE(CompleteSelectedNodeParameterTarget(document, NodeId{"missing"}, "exposure", &error)
                   .has_value());
  EXPECT_FALSE(error.empty());

  error.clear();
  EXPECT_FALSE(ProjectSelectedNodePanelFields(document, NodeId{"missing"}, 1, &sentinel, &error));
  EXPECT_EQ(sentinel.session_generation, 99u);
  ASSERT_EQ(sentinel.fields.size(), 1u);
  EXPECT_EQ(sentinel.fields.front().field_key, "leftover");

  auto* grade = document.PrimaryGrade();
  ASSERT_NE(grade, nullptr);
  const auto* exposure_id = grade->FindAdjustmentIdByType(type_ids::Exposure());
  ASSERT_NE(exposure_id, nullptr);
  grade->RemoveAdjustment(*exposure_id);
  EditorPanelProjection after_remove = sentinel;
  error.clear();
  EXPECT_FALSE(CompleteSelectedNodeParameterTarget(document, NodeId{"grade.primary"}, "exposure",
                                                   &error)
                   .has_value());
  EXPECT_FALSE(error.empty());
  error.clear();
  EXPECT_FALSE(ProjectSelectedNodePanelFields(document, NodeId{"grade.primary"}, 1, &after_remove,
                                              &error));
  EXPECT_EQ(after_remove.session_generation, 99u);
  EXPECT_EQ(after_remove.fields.front().field_key, "leftover");
}

TEST(EditorAdjustmentContextTest, WrongOwnerAndGeometryCapabilityFailExplicitly) {
  auto document = CreateDefaultPipelineDocument();
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.b"}).empty());

  std::string error;
  EXPECT_FALSE(
      CompleteSelectedNodeParameterTarget(document, NodeId{"drt"}, "exposure", &error).has_value());
  EXPECT_FALSE(error.empty());

  error.clear();
  EXPECT_FALSE(CompleteSelectedNodeParameterTarget(document, NodeId{"grade.b"}, "crop_rotate",
                                                   &error)
                   .has_value());
  EXPECT_FALSE(error.empty());

  error.clear();
  const auto geometry =
      CompleteSelectedNodeParameterTarget(document, NodeId{"develop"}, "crop_rotate", &error);
  ASSERT_TRUE(geometry.has_value()) << error;
  EXPECT_EQ(geometry->owner_kind, EditorParameterOwnerKind::Document);
  EXPECT_TRUE(geometry->node_id.Empty());
  EXPECT_TRUE(geometry->adjustment_instance_id.Empty());
}

TEST(EditorAdjustmentContextTest, SelectedNodePanelProjectionDoesNotCallModelJsonOrFullDto) {
  auto document = CreateDefaultPipelineDocument();
  auto counted  = std::make_unique<SerializationCountingModel>();
  counted->SetValue(3.0f);
  auto* observed = counted.get();
  document.InsertAdjustment(NodeId{"grade.primary"}, document.PrimaryGrade()->AdjustmentCount(),
                            AdjustmentInstanceId{"counted"}, std::move(counted));

  OperatorModelFullDtoCopyCount::Reset();
  EditorPanelProjection projection;
  std::string           error;
  ASSERT_TRUE(ProjectSelectedNodePanelFields(document, NodeId{"grade.primary"}, 4, &projection,
                                             &error))
      << error;
  EXPECT_EQ(observed->json_reads, 0);
  EXPECT_EQ(observed->dto_reads, 0);
  EXPECT_EQ(observed->loads, 0);
  EXPECT_EQ(OperatorModelFullDtoCopyCount::Peek(), 0);
  ASSERT_TRUE(ScalarOf(projection, "exposure").has_value());
  EXPECT_FLOAT_EQ(*ScalarOf(projection, "exposure"), kDefaultPipelineExposureEv);
}

TEST(EditorAdjustmentContextTest, ImageExifDisplayCopiesOwnerFieldsAndRejectsInvalidValues) {
  ExifDisplayMetaData valid;
  valid.shutter_speed_ = {1, 125};
  valid.iso_           = 200;
  valid.aperture_      = 2.8f;
  valid.focal_         = 50.0f;
  const auto display   = ReadEditorImageExifDisplay(valid);
  ASSERT_TRUE(display.shutter_speed.has_value());
  EXPECT_EQ(display.shutter_speed->first, 1);
  EXPECT_EQ(display.shutter_speed->second, 125);
  ASSERT_TRUE(display.iso.has_value());
  EXPECT_EQ(*display.iso, 200u);
  ASSERT_TRUE(display.aperture.has_value());
  EXPECT_FLOAT_EQ(*display.aperture, 2.8f);
  ASSERT_TRUE(display.focal_mm.has_value());
  EXPECT_FLOAT_EQ(*display.focal_mm, 50.0f);

  ExifDisplayMetaData invalid;
  invalid.shutter_speed_ = {0, 125};
  invalid.iso_           = 0;
  invalid.aperture_      = 0.0f;
  invalid.focal_         = -10.0f;
  const auto rejected    = ReadEditorImageExifDisplay(invalid);
  EXPECT_FALSE(rejected.shutter_speed.has_value());
  EXPECT_FALSE(rejected.iso.has_value());
  EXPECT_FALSE(rejected.aperture.has_value());
  EXPECT_FALSE(rejected.focal_mm.has_value());

  Image image;
  image.exif_display_ = valid;
  EXPECT_FALSE(ReadEditorImageExifDisplay(image).iso.has_value());
  image.has_exif_display_.store(true);
  const auto from_image = ReadEditorImageExifDisplay(image);
  ASSERT_TRUE(from_image.iso.has_value());
  EXPECT_EQ(*from_image.iso, 200u);
}

TEST(EditorAdjustmentContextTest, ContextCopiesCallerExifAndDoesNotRereadOnNodeChange) {
  auto document = CreateDefaultPipelineDocument();
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.b"}).empty());
  EditorImageExifDisplay exif;
  exif.iso = 800;
  const auto primary = MakeEditorAdjustmentContext(document, NodeId{"grade.primary"}, exif);
  const auto extra   = MakeEditorAdjustmentContext(document, NodeId{"grade.b"}, exif);
  ASSERT_TRUE(primary.has_value());
  ASSERT_TRUE(extra.has_value());
  EXPECT_EQ(primary->node_kind, EditorNodeKind::ColorGrade);
  EXPECT_EQ(extra->node_kind, EditorNodeKind::ColorGrade);
  ASSERT_TRUE(primary->exif.iso.has_value());
  ASSERT_TRUE(extra->exif.iso.has_value());
  EXPECT_EQ(*primary->exif.iso, 800u);
  EXPECT_EQ(*extra->exif.iso, 800u);
  EXPECT_EQ(primary->selected_node_id, NodeId{"grade.primary"});
  EXPECT_EQ(extra->selected_node_id, NodeId{"grade.b"});
  EXPECT_FALSE(MakeEditorAdjustmentContext(document, NodeId{"missing"}, exif).has_value());
}

TEST(EditorAdjustmentContextTest, FormatsValidShutterIsoApertureAndActualFocalMm) {
  EditorImageExifDisplay display;
  display.shutter_speed = std::pair<int, int>{1, 250};
  display.iso           = 100;
  display.aperture       = 2.8f;
  display.focal_mm      = 50.0f;
  const auto text       = FormatEditorImageExifDisplay(display);
  EXPECT_EQ(text.shutter, "1/250 s");
  EXPECT_EQ(text.iso, "100");
  EXPECT_EQ(text.aperture, "f/2.8");
  EXPECT_EQ(text.focal, "50 mm");
}

TEST(EditorAdjustmentContextTest, FormatsWholeSecondShutterAndStripsTrailingFocalZeros) {
  EditorImageExifDisplay display;
  display.shutter_speed = std::pair<int, int>{2, 1};
  display.iso           = 800;
  display.aperture       = 1.4f;
  display.focal_mm      = 35.0f;
  const auto text       = FormatEditorImageExifDisplay(display);
  EXPECT_EQ(text.shutter, "2 s");
  EXPECT_EQ(text.iso, "800");
  EXPECT_EQ(text.aperture, "f/1.4");
  EXPECT_EQ(text.focal, "35 mm");
}

TEST(EditorAdjustmentContextTest, MissingAndInvalidExifRowsUseEmDash) {
  const auto empty = FormatEditorImageExifDisplay({});
  EXPECT_EQ(empty.shutter, kMissingExifDisplay);
  EXPECT_EQ(empty.iso, kMissingExifDisplay);
  EXPECT_EQ(empty.aperture, kMissingExifDisplay);
  EXPECT_EQ(empty.focal, kMissingExifDisplay);

  ExifDisplayMetaData metadata;
  metadata.shutter_speed_ = {0, 0};
  metadata.iso_           = 0;
  metadata.aperture_     = 0.0f;
  metadata.focal_         = 0.0f;
  metadata.focal_35mm_    = 75.0f;
  const auto from_meta    = ReadEditorImageExifDisplay(metadata);
  EXPECT_FALSE(from_meta.focal_mm.has_value());
  const auto text = FormatEditorImageExifDisplay(from_meta);
  EXPECT_EQ(text.shutter, kMissingExifDisplay);
  EXPECT_EQ(text.iso, kMissingExifDisplay);
  EXPECT_EQ(text.aperture, kMissingExifDisplay);
  EXPECT_EQ(text.focal, kMissingExifDisplay);
}

TEST(EditorAdjustmentContextTest, HeaderFocalUsesActualMmAndIgnoresThirtyFiveMmEquivalent) {
  ExifDisplayMetaData metadata;
  metadata.focal_      = 50.0f;
  metadata.focal_35mm_ = 75.0f;
  metadata.iso_        = 200;
  const auto display    = ReadEditorImageExifDisplay(metadata);
  ASSERT_TRUE(display.focal_mm.has_value());
  EXPECT_FLOAT_EQ(*display.focal_mm, 50.0f);
  const auto text = FormatEditorImageExifDisplay(display);
  EXPECT_EQ(text.focal, "50 mm");
  EXPECT_EQ(text.iso, "200");
  EXPECT_TRUE(text.focal.find("75") == std::string::npos);
}

TEST(EditorAdjustmentContextTest, CapabilityRegistryMatchesNodeKind) {
  EXPECT_EQ(DefaultAdjustmentPanel(EditorNodeKind::Develop), kAdjustmentPanelRaw);
  EXPECT_EQ(DefaultAdjustmentPanel(EditorNodeKind::ColorGrade), kAdjustmentPanelTone);
  EXPECT_EQ(DefaultAdjustmentPanel(EditorNodeKind::Drt), kAdjustmentPanelDisplay);
  EXPECT_TRUE(AdjustmentPanelIsSupported(EditorNodeKind::Develop, kAdjustmentPanelGeometry));
  EXPECT_FALSE(AdjustmentPanelIsSupported(EditorNodeKind::ColorGrade, kAdjustmentPanelGeometry));
  EXPECT_FALSE(AdjustmentPanelIsSupported(EditorNodeKind::Drt, kAdjustmentPanelGeometry));
  EXPECT_TRUE(AdjustmentPanelIsSupported(EditorNodeKind::ColorGrade, kAdjustmentPanelMasks));
  EXPECT_TRUE(AdjustmentPanelIsSupported(EditorNodeKind::Drt, kAdjustmentPanelDetail));
  EXPECT_TRUE(AdjustmentFieldIsSupported(EditorNodeKind::Develop, "crop_rotate"));
  EXPECT_FALSE(AdjustmentFieldIsSupported(EditorNodeKind::ColorGrade, "crop_rotate"));
  EXPECT_TRUE(AdjustmentFieldIsSupported(EditorNodeKind::ColorGrade, "exposure"));
  EXPECT_TRUE(AdjustmentFieldIsSupported(EditorNodeKind::Drt, "odt"));
}

}  // namespace alcedo
