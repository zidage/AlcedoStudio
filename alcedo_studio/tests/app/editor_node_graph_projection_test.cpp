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
  grade->AddMask(MakeMask(MaskId{"mask.linear"}, LinearGradientMaskSource{}), 1);

  const auto snapshot = EditorNodeGraphProjection::Build(document, 1, 2, 3);
  ASSERT_EQ(snapshot.nodes.size(), 3u);
  const auto& masks = snapshot.nodes[1].masks;
  ASSERT_EQ(masks.size(), 2u);
  EXPECT_EQ(masks[0], (EditorNodeMaskProjection{MaskId{"mask.radial"}, MaskSourceKind::Radial}));
  EXPECT_EQ(masks[1],
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

TEST(EditorNodeGraphProjection, ProjectNodeCopiesStoredMaskOrderForDetachedGrades) {
  const auto  document = CreateDefaultPipelineDocument();
  const auto* primary  = document.PrimaryGrade();
  ASSERT_NE(primary, nullptr);
  const auto  from_build = EditorNodeGraphProjection::Build(document, 1, 1, 1).nodes[1];
  const auto  from_node  = EditorNodeGraphProjection::ProjectNode(*primary);
  EXPECT_EQ(from_node, from_build);

  auto extra = CreateCleanColorGradeNode(NodeId{"grade.detached"});
  extra->AddMask(MakeMask(MaskId{"mask.radial"}, RadialMaskSource{}), 0);
  extra->AddMask(MakeMask(MaskId{"mask.linear"}, LinearGradientMaskSource{}), 1);
  const auto projected = EditorNodeGraphProjection::ProjectNode(*extra);
  EXPECT_EQ(projected.node_id, NodeId{"grade.detached"});
  EXPECT_EQ(projected.node_kind, EditorNodeKind::ColorGrade);
  ASSERT_EQ(projected.masks.size(), 2u);
  EXPECT_EQ(projected.masks[0].mask_id, MaskId{"mask.radial"});
  EXPECT_EQ(projected.masks[1].mask_id, MaskId{"mask.linear"});
}

TEST(EditorNodeGraphProjection, InvalidBackboneIsRejected) {
  EXPECT_THROW((void)EditorNodeGraphProjection::Build(PipelineDocument{}, 1, 1, 1),
               std::invalid_argument);
}

TEST(EditorNodeGraphProjection, MaskGroupsFollowBackboneExecutionOrder) {
  auto document = CreateDefaultPipelineDocument();
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.last"}).empty());
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"grade.primary"}, NodeId{"grade.first"}).empty());

  const auto snapshot = EditorNodeGraphProjection::BuildMaskGroups(document, 9, 4, 7);
  EXPECT_EQ(snapshot.session_generation, 9u);
  EXPECT_EQ(snapshot.projection_revision, 4u);
  EXPECT_EQ(snapshot.topology_revision, 7u);
  ASSERT_EQ(snapshot.groups.size(), 3u);
  EXPECT_EQ(snapshot.groups[0].node_id, NodeId{"grade.first"});
  EXPECT_EQ(snapshot.groups[1].node_id, NodeId{"grade.primary"});
  EXPECT_EQ(snapshot.groups[2].node_id, NodeId{"grade.last"});
}

TEST(EditorNodeGraphProjection, MaskGroupsIncludeGradesWithoutMasks) {
  auto document = CreateDefaultPipelineDocument();
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.empty"}).empty());
  document.PrimaryGrade()->AddMask(MakeMask(MaskId{"mask.one"}, RadialMaskSource{}), 0);

  const auto snapshot = EditorNodeGraphProjection::BuildMaskGroups(document, 1, 1, 1);
  ASSERT_EQ(snapshot.groups.size(), 2u);
  EXPECT_EQ(snapshot.groups[0].node_id, NodeId{"grade.primary"});
  ASSERT_EQ(snapshot.groups[0].masks.size(), 1u);
  EXPECT_EQ(snapshot.groups[1].node_id, NodeId{"grade.empty"});
  EXPECT_TRUE(snapshot.groups[1].masks.empty());
}

TEST(EditorNodeGraphProjection, MaskGroupsCarryExactNodeIdentityAndNames) {
  auto document = CreateDefaultPipelineDocument();
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.b"}).empty());
  ASSERT_TRUE(RenameColorGrade(document, NodeId{"grade.primary"}, "Sky").empty());
  ASSERT_TRUE(RenameColorGrade(document, NodeId{"grade.b"}, "Sky").empty());
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"grade.primary"}, NodeId{"grade.top"}).empty());

  const auto snapshot = EditorNodeGraphProjection::BuildMaskGroups(document, 2, 2, 2);
  ASSERT_EQ(snapshot.groups.size(), 3u);
  // Order follows the backbone, not names: the newest grade sits on top and the
  // two grades sharing the display name keep their distinct NodeIds.
  EXPECT_EQ(snapshot.groups[0].node_id, NodeId{"grade.top"});
  EXPECT_EQ(snapshot.groups[0].display_name, "Color Grade 3");
  EXPECT_EQ(snapshot.groups[1].node_id, NodeId{"grade.primary"});
  EXPECT_EQ(snapshot.groups[1].display_name, "Sky");
  EXPECT_EQ(snapshot.groups[2].node_id, NodeId{"grade.b"});
  EXPECT_EQ(snapshot.groups[2].display_name, "Sky");
}

TEST(EditorNodeGraphProjection, MaskGroupRowsKeyMasksByNodeAndMaskId) {
  auto  document = CreateDefaultPipelineDocument();
  auto* grade    = document.PrimaryGrade();
  ASSERT_NE(grade, nullptr);
  auto radial         = MakeMask(MaskId{"mask.radial"}, RadialMaskSource{});
  radial.display_name = "Vignette";
  radial.enabled      = false;
  radial.opacity      = 0.45F;
  grade->AddMask(std::move(radial), 0);
  grade->AddMask(MakeMask(MaskId{"mask.linear"}, LinearGradientMaskSource{}), 1);
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.two"}).empty());
  auto* second = dynamic_cast<ColorGradeNodeModel*>(document.Graph().FindNode(NodeId{"grade.two"}));
  ASSERT_NE(second, nullptr);
  auto linear    = MakeMask(MaskId{"mask.other"}, LinearGradientMaskSource{});
  linear.opacity = 0.8F;
  second->AddMask(std::move(linear), 0);

  const auto snapshot = EditorNodeGraphProjection::BuildMaskGroups(document, 5, 6, 7);
  ASSERT_EQ(snapshot.groups.size(), 2u);
  ASSERT_EQ(snapshot.groups[0].masks.size(), 2u);
  const auto& first = snapshot.groups[0].masks[0];
  EXPECT_EQ(first.node_id, NodeId{"grade.primary"});
  EXPECT_EQ(first.mask_id, MaskId{"mask.radial"});
  EXPECT_EQ(first.source_kind, MaskSourceKind::Radial);
  EXPECT_EQ(first.display_name, "Vignette");
  EXPECT_FALSE(first.enabled);
  EXPECT_FLOAT_EQ(first.opacity, 0.45F);
  const auto& second_row = snapshot.groups[0].masks[1];
  EXPECT_EQ(second_row.mask_id, MaskId{"mask.linear"});
  EXPECT_EQ(second_row.source_kind, MaskSourceKind::LinearGradient);
  EXPECT_TRUE(second_row.enabled);
  EXPECT_FLOAT_EQ(second_row.opacity, 1.0F);
  ASSERT_EQ(snapshot.groups[1].masks.size(), 1u);
  EXPECT_EQ(snapshot.groups[1].masks[0].node_id, NodeId{"grade.two"});
  EXPECT_EQ(snapshot.groups[1].masks[0].mask_id, MaskId{"mask.other"});
}

TEST(EditorNodeGraphProjection, MaskGroupsReportGradeEnabledState) {
  auto document = CreateDefaultPipelineDocument();
  ASSERT_TRUE(SetColorGradeEnabled(document, NodeId{"grade.primary"}, false).empty());

  const auto snapshot = EditorNodeGraphProjection::BuildMaskGroups(document, 1, 1, 1);
  ASSERT_EQ(snapshot.groups.size(), 1u);
  EXPECT_FALSE(snapshot.groups[0].enabled);
}

TEST(EditorNodeGraphProjection, MaskGroupsOmitDetachedGrades) {
  auto document = CreateDefaultPipelineDocument();
  document.Graph().AddNode(CreateCleanColorGradeNode(NodeId{"grade.detached"}));

  const auto snapshot = EditorNodeGraphProjection::BuildMaskGroups(document, 1, 1, 1);
  ASSERT_EQ(snapshot.groups.size(), 1u);
  EXPECT_EQ(snapshot.groups[0].node_id, NodeId{"grade.primary"});
}

TEST(EditorNodeGraphProjection, MaskGroupParameterEditsDoNotRebuildGroupRows) {
  auto       document = CreateDefaultPipelineDocument();
  const auto before   = EditorNodeGraphProjection::BuildMaskGroups(document, 3, 3, 3);
  auto*      exposure = dynamic_cast<ExposureModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(4.0f);

  const auto after = EditorNodeGraphProjection::BuildMaskGroups(document, 3, 3, 3);
  EXPECT_EQ(after, before);
}

TEST(EditorNodeGraphProjection, MaskGroupsGenerationCheckMatchesSession) {
  const auto snapshot =
      EditorNodeGraphProjection::BuildMaskGroups(CreateDefaultPipelineDocument(), 15, 1, 1);

  EXPECT_TRUE(EditorNodeGraphProjection::AcceptsGeneration(snapshot, 15));
  EXPECT_FALSE(EditorNodeGraphProjection::AcceptsGeneration(snapshot, 16));
}

TEST(EditorNodeGraphProjection, MaskGroupsInvalidBackboneIsRejected) {
  EXPECT_THROW((void)EditorNodeGraphProjection::BuildMaskGroups(PipelineDocument{}, 1, 1, 1),
               std::invalid_argument);
}

}  // namespace alcedo
