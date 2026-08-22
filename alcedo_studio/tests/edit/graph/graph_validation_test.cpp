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

}  // namespace alcedo
