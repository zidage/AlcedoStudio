//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <stdexcept>
#include <vector>

#include "edit/graph/pipeline_document.hpp"

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

TEST(GpuDagModelGraph, PipelineGraphRejectsUnknownMaskPortOnColorGrade) {
  auto document = CreateDefaultPipelineDocument();
  document.Graph().Connect(NodeId{"develop"}, PortId{"image"}, NodeId{"grade.primary"},
                           PortId{"mask"});
  const auto errors = document.Graph().Validate();
  EXPECT_TRUE(HasCode(errors, GraphValidationCode::UnknownPort));
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

TEST(GpuDagModelGraph, DetachedColorGradeWithNoEdgeIsValidAndOffTheBackbone) {
  auto document = CreateDefaultPipelineDocument();
  document.Graph().AddNode(ColorGradeNodeModel::MakeDefault(NodeId{"grade.off"}));
  EXPECT_TRUE(document.Graph().Validate().empty());
  EXPECT_TRUE(document.Graph().ValidateImageBackbone().empty());
  EXPECT_TRUE(document.Graph().IsDetachedColorGrade(NodeId{"grade.off"}));
  EXPECT_FALSE(document.Graph().IsDetachedColorGrade(NodeId{"grade.primary"}));
  EXPECT_FALSE(document.Graph().IsDetachedColorGrade(NodeId{"develop"}));
  EXPECT_EQ(document.Graph().DetachedColorGradeNodeIds(), std::vector<NodeId>{NodeId{"grade.off"}});
  EXPECT_EQ(document.Graph().ImageBackboneNodeIds(),
            (std::vector<NodeId>{NodeId{"develop"}, NodeId{"grade.primary"}, NodeId{"drt"}}));
}

TEST(GpuDagModelGraph, HalfConnectedColorGradesStayInvalid) {
  auto document = CreateDefaultPipelineDocument();
  document.Graph().AddNode(ColorGradeNodeModel::MakeDefault(NodeId{"grade.half"}));
  // grade.primary keeps only its incoming edge; grade.half gets only an outgoing edge.
  document.Graph().Disconnect(NodeId{"grade.primary"}, PortId{"image"}, NodeId{"drt"},
                              PortId{"image"});
  document.Graph().Connect(NodeId{"grade.half"}, PortId{"image"}, NodeId{"drt"}, PortId{"image"});
  EXPECT_FALSE(document.Graph().IsDetachedColorGrade(NodeId{"grade.half"}));
  EXPECT_FALSE(document.Graph().IsDetachedColorGrade(NodeId{"grade.primary"}));
  EXPECT_TRUE(document.Graph().DetachedColorGradeNodeIds().empty());
  EXPECT_TRUE(HasCode(document.Graph().Validate(), GraphValidationCode::MissingRequiredInput));
  EXPECT_TRUE(HasCode(document.Graph().ValidateImageBackbone(),
                      GraphValidationCode::BrokenImageBackbone));
}

TEST(GpuDagModelGraph, ConnectedColorGradeOffTheBackboneIsReported) {
  auto document = CreateDefaultPipelineDocument();
  document.Graph().AddNode(ColorGradeNodeModel::MakeDefault(NodeId{"grade.loop"}));
  // An edge on the node itself keeps it off the Develop-to-DRT path without detaching it.
  document.Graph().Connect(NodeId{"grade.loop"}, PortId{"image"}, NodeId{"grade.loop"},
                           PortId{"image"});
  EXPECT_FALSE(document.Graph().IsDetachedColorGrade(NodeId{"grade.loop"}));
  EXPECT_TRUE(HasCode(document.Graph().ValidateImageBackbone(),
                      GraphValidationCode::ColorGradeNotOnImageBackbone));
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
