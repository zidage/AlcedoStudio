//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <cstddef>
#include <string>

#include "edit/graph/pipeline_document.hpp"
#include "edit/operators/models/adjustment_catalog.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"

namespace alcedo {

TEST(GpuDagModelGraph, DefaultPipelineHasDevelopGradeAndDrtNodes) {
  const auto document = CreateDefaultPipelineDocument();
  ASSERT_EQ(document.Graph().NodeCount(), 3u);
  ASSERT_NE(document.Develop(), nullptr);
  ASSERT_NE(document.PrimaryGrade(), nullptr);
  ASSERT_NE(document.Drt(), nullptr);
  EXPECT_EQ(document.Develop()->Type(), type_ids::DevelopNode());
  EXPECT_EQ(document.PrimaryGrade()->Type(), type_ids::ColorGradeNode());
  EXPECT_EQ(document.Drt()->Type(), type_ids::DrtNode());
  EXPECT_TRUE(document.Graph().Validate().empty());
  EXPECT_TRUE(AllowsLegacyStageAdapterRemirror(document));
}

TEST(GpuDagModelGraph, DefaultPipelineConnectsDevelopThroughPrimaryGradeToDrt) {
  const auto document = CreateDefaultPipelineDocument();
  const auto& edges   = document.Graph().Edges();
  ASSERT_EQ(edges.size(), 2u);
  EXPECT_EQ(edges[0].from_node, NodeId{"develop"});
  EXPECT_EQ(edges[0].from_port, PortId{"image"});
  EXPECT_EQ(edges[0].to_node, NodeId{"grade.primary"});
  EXPECT_EQ(edges[0].to_port, PortId{"image"});
  EXPECT_EQ(edges[1].from_node, NodeId{"grade.primary"});
  EXPECT_EQ(edges[1].from_port, PortId{"image"});
  EXPECT_EQ(edges[1].to_node, NodeId{"drt"});
  EXPECT_EQ(edges[1].to_port, PortId{"image"});

  const auto order = document.Graph().TopologicalOrder();
  ASSERT_EQ(order.size(), 3u);
  EXPECT_EQ(order[0], NodeId{"develop"});
  EXPECT_EQ(order[1], NodeId{"grade.primary"});
  EXPECT_EQ(order[2], NodeId{"drt"});
}

TEST(GpuDagModelGraph, DefaultPrimaryGradeContainsOrderedSceneAdjustments) {
  const auto document = CreateDefaultPipelineDocument();
  const auto* grade   = document.PrimaryGrade();
  ASSERT_NE(grade, nullptr);
  ASSERT_EQ(grade->AdjustmentCount(), 17u);

  const OperatorTypeId* expected[] = {
      &type_ids::Cat02WhiteBalance(), &type_ids::Exposure(),   &type_ids::Contrast(),
      &type_ids::White(),             &type_ids::Black(),      &type_ids::Shadows(),
      &type_ids::Highlights(),        &type_ids::Curve(),      &type_ids::Hls(),
      &type_ids::Saturation(),        &type_ids::Vibrance(),   &type_ids::ColorWheel(),
      &type_ids::Lmt(),               &type_ids::Clarity(),    &type_ids::Sharpen(),
      &type_ids::Halation(),          &type_ids::FilmGrain(),
  };
  for (std::size_t i = 0; i < 17; ++i) {
    EXPECT_EQ(grade->AdjustmentAt(i).Type(), *expected[i]) << i;
  }
  EXPECT_EQ(std::string{grade->AdjustmentIdAt(0).Value()}, "grade.primary.cat02_wb");
  EXPECT_EQ(std::string{grade->AdjustmentIdAt(1).Value()}, "grade.primary.exposure");
}

TEST(GpuDagModelGraph, DefaultPrimaryGradeUsesFullMixAndNoMask) {
  const auto document = CreateDefaultPipelineDocument();
  const auto* grade   = document.PrimaryGrade();
  ASSERT_NE(grade, nullptr);
  EXPECT_TRUE(grade->Enabled());
  EXPECT_FLOAT_EQ(grade->Mix(), 1.0f);
  for (const auto& edge : document.Graph().Edges()) {
    EXPECT_NE(edge.to_port, PortId{"mask"});
  }
}

TEST(GpuDagModelGraph, BuiltinCatalogTypeIdsAreUnique) {
  const auto& definitions = BuiltinAdjustmentCatalog::Instance().Definitions();
  ASSERT_GE(definitions.size(), 18u);
  for (std::size_t i = 0; i < definitions.size(); ++i) {
    for (std::size_t j = i + 1; j < definitions.size(); ++j) {
      EXPECT_NE(definitions[i].type.Text(), definitions[j].type.Text());
      EXPECT_NE(definitions[i].type.Hash(), definitions[j].type.Hash());
    }
  }
}

}  // namespace alcedo
