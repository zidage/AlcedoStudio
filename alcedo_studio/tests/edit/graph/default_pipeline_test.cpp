//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <cstddef>
#include <string>

#include "edit/graph/color_grade_node_model.hpp"
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
  ASSERT_EQ(grade->AdjustmentCount(), 13u);

  const OperatorTypeId* expected[] = {
      &type_ids::Cat02WhiteBalance(), &type_ids::Exposure(), &type_ids::Contrast(),
      &type_ids::White(),             &type_ids::Black(),    &type_ids::Shadows(),
      &type_ids::Highlights(),        &type_ids::Curve(),    &type_ids::Hls(),
      &type_ids::Saturation(),        &type_ids::Vibrance(), &type_ids::ColorWheel(),
      &type_ids::Lmt(),
  };
  for (std::size_t i = 0; i < 13; ++i) {
    EXPECT_EQ(grade->AdjustmentAt(i).Type(), *expected[i]) << i;
  }
  EXPECT_EQ(std::string{grade->AdjustmentIdAt(0).Value()}, "grade.primary.cat02_wb");
  EXPECT_EQ(std::string{grade->AdjustmentIdAt(1).Value()}, "grade.primary.exposure");
}

TEST(GpuDagModelGraph, DefaultDrtContainsOrderedPostAdjustments) {
  const auto document = CreateDefaultPipelineDocument();
  const auto* drt     = document.Drt();
  ASSERT_NE(drt, nullptr);
  ASSERT_EQ(drt->AdjustmentCount(), 4u);
  EXPECT_EQ(drt->AdjustmentAt(0).Type(), type_ids::Clarity());
  EXPECT_EQ(drt->AdjustmentAt(1).Type(), type_ids::Sharpen());
  EXPECT_EQ(drt->AdjustmentAt(2).Type(), type_ids::Halation());
  EXPECT_EQ(drt->AdjustmentAt(3).Type(), type_ids::FilmGrain());
  EXPECT_EQ(std::string{drt->AdjustmentIdAt(0).Value()}, "drt.clarity");
  EXPECT_EQ(std::string{drt->AdjustmentIdAt(1).Value()}, "drt.sharpen");
  EXPECT_EQ(std::string{drt->AdjustmentIdAt(2).Value()}, "drt.halation");
  EXPECT_EQ(std::string{drt->AdjustmentIdAt(3).Value()}, "drt.film_grain");
  for (std::size_t i = 0; i < drt->AdjustmentCount(); ++i) {
    EXPECT_TRUE(drt->AdjustmentAt(i).IsDefault()) << i;
  }
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

TEST(GpuDagModelGraph, DefaultPipelineDocumentBakesOnePointFiveEvAndSaturationOnePointThree) {
  const auto document = CreateDefaultPipelineDocument();
  const auto* grade   = document.PrimaryGrade();
  ASSERT_NE(grade, nullptr);

  const auto* exposure = dynamic_cast<const ExposureModel*>(
      grade->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  EXPECT_FLOAT_EQ(exposure->Value(), 1.5f);

  const auto* saturation = dynamic_cast<const SaturationModel*>(
      grade->FindAdjustmentByType(type_ids::Saturation()));
  ASSERT_NE(saturation, nullptr);
  EXPECT_FLOAT_EQ(saturation->Value(), 1.3f);

  EXPECT_NE(document.Graph().FindNode(NodeId{"grade.primary"}), nullptr);
  EXPECT_TRUE(document.Graph().Validate().empty());
  EXPECT_TRUE(document.Graph().ValidateImageBackbone().empty());
}

TEST(GpuDagModelGraph, MakeCleanColorGradeUsesIdentityParamsAndOmitsPostAdjustments) {
  const auto from_free   = CreateCleanColorGradeNode(NodeId{"grade.clean"});
  const auto from_static = ColorGradeNodeModel::MakeClean(NodeId{"grade.clean"});
  ASSERT_NE(from_free, nullptr);
  ASSERT_NE(from_static, nullptr);
  EXPECT_EQ(from_free->ToJson().dump(), from_static->ToJson().dump());

  const auto* grade = from_free.get();
  EXPECT_TRUE(grade->Enabled());
  EXPECT_FLOAT_EQ(grade->Mix(), 1.0f);
  ASSERT_EQ(grade->AdjustmentCount(), 13u);
  EXPECT_EQ(grade->FindAdjustmentByType(type_ids::Clarity()), nullptr);
  EXPECT_EQ(grade->FindAdjustmentByType(type_ids::Sharpen()), nullptr);
  EXPECT_EQ(grade->FindAdjustmentByType(type_ids::Halation()), nullptr);
  EXPECT_EQ(grade->FindAdjustmentByType(type_ids::FilmGrain()), nullptr);
  for (std::size_t i = 0; i < grade->AdjustmentCount(); ++i) {
    EXPECT_TRUE(grade->AdjustmentAt(i).IsDefault()) << i;
  }

  const auto* exposure = dynamic_cast<const ExposureModel*>(
      grade->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  EXPECT_FLOAT_EQ(exposure->Value(), 0.0f);
  const auto* saturation = dynamic_cast<const SaturationModel*>(
      grade->FindAdjustmentByType(type_ids::Saturation()));
  ASSERT_NE(saturation, nullptr);
  EXPECT_FLOAT_EQ(saturation->Value(), 1.0f);

  auto patched_default = ColorGradeNodeModel::MakeDefault(NodeId{"grade.clean"});
  ASSERT_EQ(patched_default->AdjustmentCount(), 13u);
  auto* default_exposure = dynamic_cast<ExposureModel*>(
      patched_default->FindAdjustmentByType(type_ids::Exposure()));
  auto* default_saturation = dynamic_cast<SaturationModel*>(
      patched_default->FindAdjustmentByType(type_ids::Saturation()));
  ASSERT_NE(default_exposure, nullptr);
  ASSERT_NE(default_saturation, nullptr);
  default_exposure->SetValue(0.0f);
  default_saturation->SetValue(1.0f);
  EXPECT_EQ(patched_default->ToJson().dump(), from_free->ToJson().dump());
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
