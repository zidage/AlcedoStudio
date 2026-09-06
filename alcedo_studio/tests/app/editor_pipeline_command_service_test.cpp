//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_pipeline_command_service.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <stdexcept>

#include "edit/graph/pipeline_document.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "support/editor_parameter_target_test.hpp"

namespace alcedo {
namespace {

/// Instrument only this unrelated Model, so repeated patches cannot hide a graph serialization.
class SerializationCountingModel : public IOperatorModel {
 public:
  mutable int reads = 0;
  int         loads = 0;
  auto        Type() const -> OperatorTypeId override { return value_.Type(); }
  auto        IsDefault() const -> bool override { return value_.IsDefault(); }
  auto        IsDirty() const -> bool override { return value_.IsDirty(); }
  auto        MakeFullDto() const -> OperatorParamDto override { return value_.MakeFullDto(); }
  auto        TakeDirtyPatch() -> std::optional<OperatorParamPatchDto> override {
    return value_.TakeDirtyPatch();
  }
  void RestoreDirty(DirtyFieldMask fields) override { value_.RestoreDirty(fields); }
  void MarkAllDirty() override { value_.MarkAllDirty(); }
  auto ToJson() const -> nlohmann::json override {
    ++reads;
    return value_.ToJson();
  }
  void LoadJson(const nlohmann::json& json) override {
    ++loads;
    value_.LoadJson(json);
  }
  [[nodiscard]] auto value() const -> float { return value_.Value(); }

 private:
  ExposureModel value_;
};

/// Simulate a multi-field setter failing after it already changed a value.
class PartiallyFailingModel final : public SerializationCountingModel {
 public:
  void LoadJson(const nlohmann::json& json) override {
    SerializationCountingModel::LoadJson(json);
    if (json.at("exposure_ev") == 5.0) throw std::runtime_error("injected setter failure");
  }
};

TEST(EditorPipelineCommandServiceTest, ThrowingSetterRestoresOnlyAffectedModelParameters) {
  auto  document          = CreateDefaultPipelineDocument();
  auto  failing           = std::make_unique<PartiallyFailingModel>();
  auto* affected          = failing.get();
  auto* primary           = document.PrimaryGrade();
  auto* original_exposure = primary->FindAdjustment(AdjustmentInstanceId{"grade.primary.exposure"});
  document.InsertAdjustment(NodeId{"grade.primary"}, 0, AdjustmentInstanceId{"failing"},
                            std::move(failing));
  const auto before             = document.ToJson();
  auto       target             = test::ColorGradeFieldTarget("exposure");
  target.adjustment_instance_id = AdjustmentInstanceId{"failing"};
  std::string error;
  EXPECT_FALSE(ApplyEditorParameterPatch(document, target, {{"exposure_ev", 5.0}}, &error));
  EXPECT_EQ(error, "injected setter failure");
  EXPECT_EQ(document.PrimaryGrade(), primary);
  EXPECT_EQ(primary->FindAdjustment(AdjustmentInstanceId{"failing"}), affected);
  EXPECT_EQ(primary->FindAdjustment(AdjustmentInstanceId{"grade.primary.exposure"}),
            original_exposure);
  EXPECT_EQ(document.ToJson(), before);
}

TEST(EditorPipelineCommandServiceTest, ParameterPatchPreservesUnchangedModels) {
  auto  document = CreateDefaultPipelineDocument();
  auto  counter  = std::make_unique<SerializationCountingModel>();
  auto* observed = counter.get();
  document.InsertAdjustment(NodeId{"grade.primary"}, 0, AdjustmentInstanceId{"counted"},
                            std::move(counter));
  std::vector<const INodeModel*>     nodes;
  std::vector<const IOperatorModel*> models;
  for (const auto& node : document.Graph().Nodes()) nodes.push_back(node.get());
  auto* grade = document.PrimaryGrade();
  for (std::size_t i = 0; i < grade->AdjustmentCount(); ++i) {
    models.push_back(&grade->AdjustmentAt(i));
    (void)grade->AdjustmentAt(i).TakeDirtyPatch();
  }
  const auto edges = document.ToJson().at("edges");
  observed->reads  = 0;
  document.ClearTopologyDirty();
  std::string error;
  for (int i = 0; i < 40; ++i) {
    ASSERT_TRUE(PublishEditorParameterPatch(document, test::ColorGradeFieldTarget("exposure"),
                                            {{"exposure_ev", i / 4.0}}, &error))
        << error;
  }
  EXPECT_EQ(observed->reads, 0);
  EXPECT_FALSE(document.TopologyDirty());
  for (std::size_t i = 0; i < nodes.size(); ++i)
    EXPECT_EQ(document.Graph().Nodes()[i].get(), nodes[i]);
  for (std::size_t i = 0; i < models.size(); ++i) EXPECT_EQ(&grade->AdjustmentAt(i), models[i]);
  EXPECT_FALSE(observed->IsDirty());
  const auto* exposure = dynamic_cast<const ExposureModel*>(
      grade->FindAdjustment(AdjustmentInstanceId{"grade.primary.exposure"}));
  ASSERT_NE(exposure, nullptr);
  EXPECT_FLOAT_EQ(exposure->Value(), 9.75f);
  EXPECT_TRUE(exposure->IsDirty());
  EXPECT_EQ(document.ToJson().at("edges"), edges);
}

TEST(EditorPipelineCommandServiceTest, ApplyingScalarPatchReadsAndReloadsTargetModelJson) {
  auto  document = CreateDefaultPipelineDocument();
  auto  counted  = std::make_unique<SerializationCountingModel>();
  auto* observed = counted.get();
  document.InsertAdjustment(NodeId{"grade.primary"}, 0, AdjustmentInstanceId{"counted"},
                            std::move(counted));

  auto target                       = test::ColorGradeFieldTarget("exposure");
  target.adjustment_instance_id    = AdjustmentInstanceId{"counted"};
  std::string error;
  ASSERT_TRUE(ApplyEditorParameterPatch(document, target, {{"exposure_ev", 2.5}}, &error))
      << error;

  EXPECT_EQ(observed->reads, 1);
  EXPECT_EQ(observed->loads, 1);
  EXPECT_FLOAT_EQ(observed->value(), 2.5f);
}

TEST(EditorPipelineCommandServiceTest,
     ReadingScalarParameterUsesModelJsonWhileDocumentPersistenceUsesDocumentJson) {
  auto  document = CreateDefaultPipelineDocument();
  auto  counted  = std::make_unique<SerializationCountingModel>();
  auto* observed = counted.get();
  document.InsertAdjustment(NodeId{"grade.primary"}, 0, AdjustmentInstanceId{"counted"},
                            std::move(counted));
  auto target                    = test::ColorGradeFieldTarget("exposure");
  target.adjustment_instance_id = AdjustmentInstanceId{"counted"};

  std::string     error;
  nlohmann::json  json;
  ASSERT_TRUE(ReadEditorParameterJson(document, target, &json, &error)) << error;
  EXPECT_EQ(observed->reads, 1);
  EXPECT_EQ(observed->loads, 0);
  EXPECT_TRUE(json.contains("exposure_ev"));

  const auto reads_after_projection = observed->reads;
  const auto persisted              = CanonicalPipelineDocumentJson(document);
  EXPECT_FALSE(persisted.empty());
  EXPECT_GT(observed->reads, reads_after_projection);
}

TEST(EditorPipelineCommandServiceTest, InvalidCompoundParameterDoesNotPartiallyApplyOrDirtyModel) {
  auto  document = CreateDefaultPipelineDocument();
  auto* model    = document.Drt()->FindAdjustmentByType(type_ids::Sharpen());
  ASSERT_NE(model, nullptr);
  const auto before = model->ToJson();
  (void)model->TakeDirtyPatch();
  std::string error;
  for (const auto& patch :
       std::vector<nlohmann::json>{{{"amount", 12}, {"radius", "invalid"}},
                                   {{"amount", nullptr}},
                                   {{"amount", 12}, {"unknown", 1}},
                                   {{"amount", std::numeric_limits<double>::infinity()}}}) {
    EXPECT_FALSE(PublishEditorParameterPatch(document, test::DrtPostFieldTarget("sharpen"),
                                             patch, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(model->ToJson(), before);
    EXPECT_FALSE(model->IsDirty());
  }
}

TEST(EditorPipelineCommandServiceTest, GeometryAndDevelopRejectInvalidValuesBeforeAnyWrite) {
  auto                  document = CreateDefaultPipelineDocument();
  const auto            before   = document.ToJson();
  std::string           error;
  EditorParameterTarget geometry;
  geometry.owner_kind = EditorParameterOwnerKind::Document;
  geometry.field_key  = "crop_rotate";
  EXPECT_FALSE(ApplyEditorParameterPatch(
      document, geometry, {{"crop_rect", {0, 0, 1, "bad"}}, {"rotation_degrees", 30}}, &error));
  EditorParameterTarget develop;
  develop.owner_kind = EditorParameterOwnerKind::Develop;
  develop.node_id    = NodeId{"develop"};
  develop.field_key  = "raw_decode";
  EXPECT_FALSE(ApplyEditorParameterPatch(
      document, develop, {{"demosaic_method", "test"}, {"user_wb", "bad"}}, &error));
  EXPECT_EQ(document.ToJson(), before);
}

TEST(EditorPipelineCommandServiceTest, SettledExposurePatchWritesPrimaryGradeDocumentNotOnlyStages) {
  auto document = CreateDefaultPipelineDocument();
  const auto before = CanonicalPipelineDocumentJson(document);
  std::string error;
  ASSERT_TRUE(PublishEditorParameterPatch(document, test::ColorGradeFieldTarget("exposure"),
                                          {{"exposure_ev", 2.25}}, &error))
      << error;
  nlohmann::json json;
  ASSERT_TRUE(ReadEditorParameterJson(document, test::ColorGradeFieldTarget("exposure"), &json,
                                      &error))
      << error;
  EXPECT_FLOAT_EQ(json.at("exposure_ev").get<float>(), 2.25f);
  EXPECT_NE(CanonicalPipelineDocumentJson(document), before);
}

TEST(EditorPipelineCommandServiceTest, IncompleteTargetIsRejectedWithoutMutation) {
  auto document = CreateDefaultPipelineDocument();
  const auto before = CanonicalPipelineDocumentJson(document);
  EditorParameterTarget target;
  target.field_key = "exposure";
  std::string error;
  EXPECT_FALSE(PublishEditorParameterPatch(document, target, {{"exposure_ev", 3.0}}, &error));
  EXPECT_EQ(error, "Editor parameter target requires owner_kind");
  EXPECT_EQ(CanonicalPipelineDocumentJson(document), before);

  auto missing_node = test::ColorGradeFieldTarget("exposure");
  missing_node.node_id = NodeId{};
  EXPECT_FALSE(
      PublishEditorParameterPatch(document, missing_node, {{"exposure_ev", 3.0}}, &error));
  EXPECT_EQ(error, "Editor parameter target requires node_id");
  EXPECT_EQ(CanonicalPipelineDocumentJson(document), before);
}

TEST(EditorPipelineCommandServiceTest, MaskTargetWriteIsRejected) {
  auto document = CreateDefaultPipelineDocument();
  const auto before = CanonicalPipelineDocumentJson(document);
  EditorParameterTarget target;
  target.owner_kind             = EditorParameterOwnerKind::ColorGradeMask;
  target.node_id                = NodeId{"grade.primary"};
  target.adjustment_instance_id = AdjustmentInstanceId{"grade.primary.exposure"};
  target.mask_id                = "mask.1";
  target.field_key              = "exposure";
  std::string error;
  EXPECT_FALSE(PublishEditorParameterPatch(document, target, {{"exposure_ev", 3.0}}, &error));
  EXPECT_EQ(error, "Mask parameter targets are rejected until NM3");
  EXPECT_EQ(CanonicalPipelineDocumentJson(document), before);
}

TEST(EditorPipelineCommandServiceTest, CompleteCurrentPanelRoutesClarityToDrtPost) {
  auto        document = CreateDefaultPipelineDocument();
  std::string error;
  const auto  target = CompleteCurrentPanelParameterTarget(document, "clarity", &error);
  ASSERT_TRUE(target.has_value()) << error;
  EXPECT_EQ(target->owner_kind, EditorParameterOwnerKind::DrtPost);
  EXPECT_EQ(target->node_id, NodeId{"drt"});
  EXPECT_EQ(target->adjustment_instance_id, AdjustmentInstanceId{"drt.clarity"});
  EXPECT_EQ(target->field_key, "clarity");

  const auto exposure = CompleteCurrentPanelParameterTarget(document, "exposure", &error);
  ASSERT_TRUE(exposure.has_value()) << error;
  EXPECT_EQ(exposure->owner_kind, EditorParameterOwnerKind::ColorGrade);
  EXPECT_EQ(exposure->node_id, NodeId{"grade.primary"});
}

TEST(EditorPipelineCommandServiceTest, PublishClarityWritesDrtModelAndRejectsGradeOwner) {
  auto        document = CreateDefaultPipelineDocument();
  std::string error;
  ASSERT_TRUE(PublishEditorParameterPatch(document, test::DrtPostFieldTarget("clarity"),
                                          {{"clarity", 40.0}}, &error))
      << error;
  nlohmann::json json;
  ASSERT_TRUE(ReadEditorParameterJson(document, test::DrtPostFieldTarget("clarity"), &json, &error))
      << error;
  EXPECT_FLOAT_EQ(json.at("clarity").get<float>(), 40.0f);
  const auto* drt_clarity = dynamic_cast<const ClarityModel*>(
      document.Drt()->FindAdjustmentByType(type_ids::Clarity()));
  ASSERT_NE(drt_clarity, nullptr);
  EXPECT_FLOAT_EQ(drt_clarity->Value(), 40.0f);
  EXPECT_EQ(document.PrimaryGrade()->FindAdjustmentByType(type_ids::Clarity()), nullptr);

  const auto before = CanonicalPipelineDocumentJson(document);
  EXPECT_FALSE(PublishEditorParameterPatch(document, test::ColorGradeFieldTarget("clarity"),
                                           {{"clarity", 12.0}}, &error));
  EXPECT_EQ(CanonicalPipelineDocumentJson(document), before);
}

TEST(EditorPipelineCommandServiceTest, MissingAdjustmentInstanceLeavesDocumentUnchanged) {
  auto document = CreateDefaultPipelineDocument();
  const auto before = CanonicalPipelineDocumentJson(document);
  auto target = test::ColorGradeFieldTarget("exposure");
  target.adjustment_instance_id = AdjustmentInstanceId{"grade.primary.missing"};
  std::string error;
  EXPECT_FALSE(PublishEditorParameterPatch(document, target, {{"exposure_ev", 3.0}}, &error));
  EXPECT_EQ(CanonicalPipelineDocumentJson(document), before);
}

}  // namespace
}  // namespace alcedo
