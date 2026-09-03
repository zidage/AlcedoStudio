//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_node_graph_projection.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <utility>

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_commands.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"

namespace alcedo {
namespace {

auto MakeMask(MaskId id, MaskSource source) -> MaskModel {
  MaskModel mask;
  mask.id           = std::move(id);
  mask.display_name = "Mask";
  mask.source       = std::move(source);
  return mask;
}

}  // namespace

TEST(EditorNodeGraphProjection, DefaultSnapshotContainsBackboneValuesAndEdges) {
  const auto document = CreateDefaultPipelineDocument();
  const auto snapshot = EditorNodeGraphProjection::Build(document, 7, 12, 4);

  EXPECT_EQ(snapshot.session_generation, 7u);
  EXPECT_EQ(snapshot.projection_revision, 12u);
  EXPECT_EQ(snapshot.topology_revision, 4u);
  ASSERT_EQ(snapshot.nodes.size(), 3u);
  EXPECT_EQ(snapshot.nodes[0].node_id, NodeId{"develop"});
  EXPECT_EQ(snapshot.nodes[0].node_kind, EditorNodeKind::Develop);
  EXPECT_EQ(snapshot.nodes[0].display_name, "Develop");
  EXPECT_EQ(snapshot.nodes[1].node_id, NodeId{"grade.primary"});
  EXPECT_EQ(snapshot.nodes[1].node_kind, EditorNodeKind::ColorGrade);
  EXPECT_EQ(snapshot.nodes[1].display_name, "Color Grade 1");
  EXPECT_TRUE(snapshot.nodes[1].masks.empty());
  EXPECT_EQ(snapshot.nodes[2].node_id, NodeId{"drt"});
  EXPECT_EQ(snapshot.nodes[2].node_kind, EditorNodeKind::Drt);
  EXPECT_EQ(snapshot.nodes[2].display_name, "DRT");

  ASSERT_EQ(snapshot.edges.size(), 2u);
  EXPECT_EQ(snapshot.edges[0].source_node_id, NodeId{"develop"});
  EXPECT_EQ(snapshot.edges[0].source_port_id, PortId{"image"});
  EXPECT_EQ(snapshot.edges[0].destination_node_id, NodeId{"grade.primary"});
  EXPECT_EQ(snapshot.edges[0].destination_port_id, PortId{"image"});
  EXPECT_EQ(snapshot.edges[1].source_node_id, NodeId{"grade.primary"});
  EXPECT_EQ(snapshot.edges[1].destination_node_id, NodeId{"drt"});
}

TEST(EditorNodeGraphProjection, MaskProjectionPreservesDocumentDisplayOrderAndSourceKinds) {
  auto  document = CreateDefaultPipelineDocument();
  auto* grade    = document.PrimaryGrade();
  ASSERT_NE(grade, nullptr);
  grade->AddMask(MakeMask(MaskId{"mask.radial"}, RadialMaskSource{}), 0);
  grade->AddMask(MakeMask(MaskId{"mask.brush"}, BrushMaskSource{}), 1);
  grade->AddMask(MakeMask(MaskId{"mask.linear"}, LinearGradientMaskSource{}), 2);

  const auto snapshot = EditorNodeGraphProjection::Build(document, 1, 2, 3);
  ASSERT_EQ(snapshot.nodes.size(), 3u);
  const auto& masks = snapshot.nodes[1].masks;
  ASSERT_EQ(masks.size(), 3u);
  EXPECT_EQ(masks[0], (EditorNodeMaskProjection{MaskId{"mask.radial"}, MaskSourceKind::Radial}));
  EXPECT_EQ(masks[1], (EditorNodeMaskProjection{MaskId{"mask.brush"}, MaskSourceKind::Brush}));
  EXPECT_EQ(masks[2],
            (EditorNodeMaskProjection{MaskId{"mask.linear"}, MaskSourceKind::LinearGradient}));
}

TEST(EditorNodeGraphProjection, ParameterChangeDoesNotChangeSnapshotValues) {
  auto       document = CreateDefaultPipelineDocument();
  const auto before   = EditorNodeGraphProjection::Build(document, 3, 8, 9);
  auto*      exposure = dynamic_cast<ExposureModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(4.0f);

  const auto after = EditorNodeGraphProjection::Build(document, 3, 8, 9);
  EXPECT_EQ(after, before);
}

TEST(EditorNodeGraphProjection, GenerationCheckRejectsSnapshotFromAnotherSession) {
  const auto snapshot = EditorNodeGraphProjection::Build(CreateDefaultPipelineDocument(), 15, 1, 1);

  EXPECT_TRUE(EditorNodeGraphProjection::AcceptsGeneration(snapshot, 15));
  EXPECT_FALSE(EditorNodeGraphProjection::AcceptsGeneration(snapshot, 16));
}

TEST(EditorNodeGraphProjection, TopologyChangeAppearsInNodeAndRevisionValues) {
  auto document = CreateDefaultPipelineDocument();
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.second"}).empty());

  const auto snapshot = EditorNodeGraphProjection::Build(document, 2, 5, 6);
  ASSERT_EQ(snapshot.nodes.size(), 4u);
  EXPECT_EQ(snapshot.nodes[2].node_id, NodeId{"grade.second"});
  EXPECT_EQ(snapshot.nodes[2].display_name, "Color Grade 2");
  EXPECT_EQ(snapshot.topology_revision, 6u);
  EXPECT_EQ(snapshot.edges.size(), 3u);
}

TEST(EditorNodeGraphProjection, InvalidBackboneIsRejected) {
  EXPECT_THROW(EditorNodeGraphProjection::Build(PipelineDocument{}, 1, 1, 1),
               std::invalid_argument);
}

}  // namespace alcedo
