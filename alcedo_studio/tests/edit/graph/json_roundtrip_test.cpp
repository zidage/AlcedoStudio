//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_commands.hpp"
#include "edit/operators/models/adjustment_catalog.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/operators/models/sharpen_model.hpp"
#include "grade_owned_mask_support.hpp"
#include "json.hpp"

namespace alcedo {

TEST(GpuDagModelGraph, BrushMaskRoundTripPreservesMaskAssetKey) {
  auto document = CreateDefaultPipelineDocument();
  grade_mask_test::AddBrushMask(document, MaskId{"mask.persisted"}, MaskAssetKey{"asset_01"});
  const auto  restored = PipelineDocument::FromJson(document.ToJson());
  const auto* restored_mask = restored.PrimaryGrade()->FindMask(MaskId{"mask.persisted"});
  ASSERT_NE(restored_mask, nullptr);
  const auto* brush = std::get_if<BrushMaskSource>(&restored_mask->source);
  ASSERT_NE(brush, nullptr);
  ASSERT_TRUE(brush->asset_key.has_value());
  EXPECT_EQ(*brush->asset_key, MaskAssetKey{"asset_01"});
}

TEST(GpuDagModelGraph, PipelineDocumentRoundTripPreservesNodeIdsEdgesAndAdjustmentOrder) {
  auto  document = CreateDefaultPipelineDocument();
  auto* exposure = dynamic_cast<ExposureModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(1.25f);
  document.PrimaryGrade()->MoveAdjustment(AdjustmentInstanceId{"grade.primary.lmt"}, 8);

  const auto json = document.ToJson();
  EXPECT_EQ(json["format_version"], kPipelineDocumentFormatVersion);
  EXPECT_FALSE(json.dump().find("stage") != std::string::npos && json.contains("stage"));
  EXPECT_FALSE(json.contains("priority"));
  const std::string dumped = json.dump();
  EXPECT_EQ(dumped.find("\"priority\""), std::string::npos);
  EXPECT_EQ(dumped.find("Image Loading"), std::string::npos);

  const auto restored = PipelineDocument::FromJson(json);
  ASSERT_EQ(restored.Graph().NodeCount(), 3u);
  ASSERT_NE(restored.Develop(), nullptr);
  ASSERT_NE(restored.PrimaryGrade(), nullptr);
  ASSERT_NE(restored.Drt(), nullptr);
  ASSERT_EQ(restored.Graph().Edges().size(), 2u);
  EXPECT_EQ(restored.Graph().Edges()[0].from_node, NodeId{"develop"});
  EXPECT_EQ(restored.Graph().Edges()[1].to_node, NodeId{"drt"});

  const auto* grade = restored.PrimaryGrade();
  ASSERT_EQ(grade->AdjustmentCount(), 13u);
  EXPECT_EQ(std::string{grade->AdjustmentIdAt(8).Value()}, "grade.primary.lmt");
  const auto* restored_exposure = dynamic_cast<const ExposureModel*>(&grade->AdjustmentAt(1));
  ASSERT_NE(restored_exposure, nullptr);
  EXPECT_FLOAT_EQ(restored_exposure->Value(), 1.25f);
  const auto* restored_drt = restored.Drt();
  ASSERT_NE(restored_drt, nullptr);
  ASSERT_EQ(restored_drt->AdjustmentCount(), 4u);
  EXPECT_EQ(std::string{restored_drt->AdjustmentIdAt(3).Value()}, "drt.film_grain");
  EXPECT_TRUE(restored.Graph().Validate().empty());
}

TEST(GpuDagModelGraph, MultiGradeJsonRoundTripPreservesOwnersAndEdges) {
  auto document = CreateDefaultPipelineDocument();
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.b"}).empty());
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.c"}).empty());
  auto* exposure = dynamic_cast<ExposureModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  auto* grade_b = dynamic_cast<ColorGradeNodeModel*>(document.Graph().FindNode(NodeId{"grade.b"}));
  ASSERT_NE(grade_b, nullptr);
  auto* contrast = dynamic_cast<ContrastModel*>(grade_b->FindAdjustmentByType(type_ids::Contrast()));
  auto* clarity = dynamic_cast<ClarityModel*>(
      document.Drt()->FindAdjustmentByType(type_ids::Clarity()));
  auto* sharpen = dynamic_cast<SharpenModel*>(
      document.Drt()->FindAdjustmentByType(type_ids::Sharpen()));
  ASSERT_NE(exposure, nullptr);
  ASSERT_NE(contrast, nullptr);
  ASSERT_NE(clarity, nullptr);
  ASSERT_NE(sharpen, nullptr);
  exposure->SetValue(0.75f);
  contrast->SetValue(40.0f);
  clarity->SetValue(25.0f);
  sharpen->SetAmount(12.0f);

  const auto json = document.ToJson();
  EXPECT_FALSE(json.contains("stages"));
  const auto restored = PipelineDocument::FromJson(json);
  EXPECT_EQ(restored.Graph().ImageBackboneNodeIds(),
            (std::vector<NodeId>{NodeId{"develop"}, NodeId{"grade.primary"}, NodeId{"grade.b"},
                                 NodeId{"grade.c"}, NodeId{"drt"}}));
  EXPECT_EQ(restored.PrimaryGrade()->FindAdjustmentByType(type_ids::Clarity()), nullptr);
  EXPECT_NE(restored.Drt()->FindAdjustmentByType(type_ids::Clarity()), nullptr);
  EXPECT_FLOAT_EQ(dynamic_cast<const ExposureModel*>(
                      restored.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()))
                      ->Value(),
                  0.75f);
  EXPECT_FLOAT_EQ(dynamic_cast<const ClarityModel*>(
                      restored.Drt()->FindAdjustmentByType(type_ids::Clarity()))
                      ->Value(),
                  25.0f);

  auto malformed = json;
  auto default_clarity =
      BuiltinAdjustmentCatalog::Instance().CreateDefault(type_ids::Clarity());
  for (auto& node : malformed["nodes"]) {
    if (node.at("id") == "grade.primary") {
      node["adjustments"].push_back({{"id", "grade.primary.clarity"},
                                     {"type", std::string{type_ids::Clarity().Text()}},
                                     {"params", default_clarity->ToJson()}});
    }
  }
  EXPECT_THROW((void)PipelineDocument::FromJson(malformed), std::runtime_error);
}

}  // namespace alcedo
