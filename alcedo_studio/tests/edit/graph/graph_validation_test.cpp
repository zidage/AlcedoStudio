//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <stdexcept>
#include <vector>

#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/raster_mask_node_model.hpp"

namespace alcedo {

namespace {

auto HasCode(const std::vector<GraphValidationError>& errors, GraphValidationCode code) -> bool {
  for (const auto& error : errors) {
    if (error.code == code) {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST(GpuDagModelGraph, PipelineGraphRejectsCycle) {
  auto document = CreateDefaultPipelineDocument();
  document.Graph().AddNode(ColorGradeNodeModel::MakeDefault(NodeId{"grade.b"}));
  document.Graph().Connect(NodeId{"grade.primary"}, PortId{"image"}, NodeId{"grade.b"},
                           PortId{"image"});
  document.Graph().Connect(NodeId{"grade.b"}, PortId{"image"}, NodeId{"grade.primary"},
                           PortId{"image"});
  const auto errors = document.Graph().Validate();
  EXPECT_TRUE(HasCode(errors, GraphValidationCode::Cycle));
  EXPECT_THROW(
      {
        const auto order = document.Graph().TopologicalOrder();
        (void)order;
      },
      std::runtime_error);
}

TEST(GpuDagModelGraph, PipelineGraphRejectsDisplayImageConnectedToSceneInput) {
  auto document = CreateDefaultPipelineDocument();
  document.Graph().Connect(NodeId{"drt"}, PortId{"display"}, NodeId{"grade.primary"},
                           PortId{"image"});
  const auto errors = document.Graph().Validate();
  EXPECT_TRUE(HasCode(errors, GraphValidationCode::PortTypeMismatch));
}

TEST(GpuDagModelGraph, PipelineGraphRejectsMaskConnectedToImageInput) {
  auto document = CreateDefaultPipelineDocument();
  document.Graph().AddNode(std::make_unique<RasterMaskNodeModel>(NodeId{"mask.1"}));
  document.Graph().Connect(NodeId{"mask.1"}, PortId{"mask"}, NodeId{"grade.primary"},
                           PortId{"image"});
  const auto errors = document.Graph().Validate();
  EXPECT_TRUE(HasCode(errors, GraphValidationCode::PortTypeMismatch));
}

TEST(GpuDagModelGraph, DefaultDocumentSatisfiesImageBackbone) {
  const auto document = CreateDefaultPipelineDocument();
  EXPECT_TRUE(document.Graph().Validate().empty());
  EXPECT_TRUE(document.Graph().ValidateImageBackbone().empty());
  const auto path = document.Graph().ImageBackboneNodeIds();
  ASSERT_EQ(path.size(), 3u);
  EXPECT_EQ(path[0], NodeId{"develop"});
  EXPECT_EQ(path[1], NodeId{"grade.primary"});
  EXPECT_EQ(path[2], NodeId{"drt"});
}

TEST(GpuDagModelGraph, ValidateImageBackboneRejectsColorGradeOffTheImageBackbone) {
  auto document = CreateDefaultPipelineDocument();
  document.Graph().AddNode(ColorGradeNodeModel::MakeDefault(NodeId{"grade.off"}));
  EXPECT_TRUE(HasCode(document.Graph().Validate(), GraphValidationCode::MissingRequiredInput));
  const auto errors = document.Graph().ValidateImageBackbone();
  EXPECT_TRUE(HasCode(errors, GraphValidationCode::ColorGradeNotOnImageBackbone));
}

TEST(GpuDagModelGraph, ValidateImageBackboneRejectsSceneImageFanOut) {
  auto document = CreateDefaultPipelineDocument();
  document.Graph().AddNode(ColorGradeNodeModel::MakeDefault(NodeId{"grade.b"}));
  document.Graph().Connect(NodeId{"grade.primary"}, PortId{"image"}, NodeId{"grade.b"},
                           PortId{"image"});
  EXPECT_TRUE(document.Graph().Validate().empty());
  const auto errors = document.Graph().ValidateImageBackbone();
  EXPECT_TRUE(HasCode(errors, GraphValidationCode::SceneImageFanOut));
  EXPECT_TRUE(HasCode(errors, GraphValidationCode::BrokenImageBackbone));
}

}  // namespace alcedo
