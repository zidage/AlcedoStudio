//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <cstddef>
#include <string>

#include "edit/graph/analytic_mask_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/operators/models/adjustment_catalog.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"

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

TEST(GpuDagModelGraph, TemporaryOvalMaskConnectsToPrimaryGradeMaskPort) {
  auto document = CreateDefaultPipelineDocument();
  ASSERT_EQ(document.Graph().NodeCount(), 3u);
  AttachTemporaryPrimaryGradeOvalMask(document);
  AttachTemporaryPrimaryGradeOvalMask(document);

  ASSERT_EQ(document.Graph().NodeCount(), 4u);
  EXPECT_TRUE(document.Graph().Validate().empty());
  const auto* mask = dynamic_cast<const AnalyticMaskNodeModel*>(
      document.Graph().FindNode(NodeId{"mask.ui_test.radial"}));
  ASSERT_NE(mask, nullptr);
  EXPECT_EQ(mask->Kind(), AnalyticMaskKind::Radial);
  EXPECT_FLOAT_EQ(mask->Radial().center_x, 0.5f);
  EXPECT_FLOAT_EQ(mask->Radial().center_y, 0.5f);
  EXPECT_FLOAT_EQ(mask->Radial().major_radius, 0.32f);
  EXPECT_FLOAT_EQ(mask->Radial().minor_radius, 0.20f);
  EXPECT_FLOAT_EQ(mask->Radial().outer_feather, 0.12f);

  int mask_edges = 0;
  for (const auto& edge : document.Graph().Edges()) {
    if (edge.to_node == NodeId{"grade.primary"} && edge.to_port == PortId{"mask"}) {
      EXPECT_EQ(edge.from_node, NodeId{"mask.ui_test.radial"});
      EXPECT_EQ(edge.from_port, PortId{"mask"});
      ++mask_edges;
    }
  }
  EXPECT_EQ(mask_edges, 1);

  const auto* grade = document.PrimaryGrade();
  ASSERT_NE(grade, nullptr);
  EXPECT_FLOAT_EQ(grade->Mix(), 1.0f);
  const auto* exposure =
      dynamic_cast<const ExposureModel*>(grade->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  EXPECT_FLOAT_EQ(exposure->Value(), 1.0f);
}

TEST(GpuDagModelGraph, TemporaryOvalMaskKeepsNonDefaultExposureAndAllowsLegacyRemirror) {
  auto document = CreateDefaultPipelineDocument();
  EXPECT_TRUE(AllowsLegacyStageAdapterRemirror(document));
  AttachTemporaryPrimaryGradeOvalMask(document);
  EXPECT_TRUE(AllowsLegacyStageAdapterRemirror(document));

  auto* exposure =
      dynamic_cast<ExposureModel*>(document.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(2.25f);
  AttachTemporaryPrimaryGradeOvalMask(document);
  EXPECT_FLOAT_EQ(exposure->Value(), 2.25f);
  EXPECT_EQ(document.Graph().NodeCount(), 4u);
  EXPECT_TRUE(document.Graph().Validate().empty());
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
