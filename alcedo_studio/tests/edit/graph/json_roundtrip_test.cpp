//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <string>

#include "edit/graph/pipeline_document.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"

namespace alcedo {

TEST(GpuDagModelGraph, PipelineDocumentRoundTripPreservesNodeIdsEdgesAndAdjustmentOrder) {
  auto document = CreateDefaultPipelineDocument();
  auto* exposure = dynamic_cast<ExposureModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(1.25f);
  document.PrimaryGrade()->MoveAdjustment(AdjustmentInstanceId{"grade.primary.film_grain"}, 8);

  const auto json     = document.ToJson();
  EXPECT_EQ(json["format_version"], 2);
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
  ASSERT_EQ(grade->AdjustmentCount(), 17u);
  EXPECT_EQ(std::string{grade->AdjustmentIdAt(8).Value()}, "grade.primary.film_grain");
  const auto* restored_exposure =
      dynamic_cast<const ExposureModel*>(&grade->AdjustmentAt(1));
  ASSERT_NE(restored_exposure, nullptr);
  EXPECT_FLOAT_EQ(restored_exposure->Value(), 1.25f);
  EXPECT_TRUE(restored.Graph().Validate().empty());
}

}  // namespace alcedo
