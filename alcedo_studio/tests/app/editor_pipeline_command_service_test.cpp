//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_pipeline_command_service.hpp"

#include <gtest/gtest.h>

#include "edit/graph/pipeline_document.hpp"
#include "support/editor_parameter_target_test.hpp"

namespace alcedo {
namespace {

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
