//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <string>

#include "app/editor_node_graph_draft.hpp"
#include "app/pipeline_document_history.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/graph_validation.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_commands.hpp"
#include "edit/mask/mask_model.hpp"

namespace {

using alcedo::AddCleanColorGrade;
using alcedo::ApplyNodeGraphTopologyChange;
using alcedo::ClonePipelineDocument;
using alcedo::ColorGradeNodeModel;
using alcedo::CreateDefaultPipelineDocument;
using alcedo::EditorNodeGraphDraft;
using alcedo::EditorNodeGraphDraftIdentity;
using alcedo::MaskId;
using alcedo::MaskModel;
using alcedo::NodeGraphDraftIssue;
using alcedo::NodeId;
using alcedo::PipelineDocument;
using alcedo::PipelineEditApplyDirection;
using alcedo::RadialMaskSource;

auto BoundIdentity() -> EditorNodeGraphDraftIdentity {
  EditorNodeGraphDraftIdentity identity;
  identity.element_id          = 8;
  identity.image_id            = 9;
  identity.version_id          = "v1";
  identity.session_generation  = 4;
  identity.projection_revision = 1;
  identity.topology_revision   = 1;
  return identity;
}

auto MakeMask(const std::string& id, float center) -> MaskModel {
  MaskModel mask;
  mask.id           = MaskId{id};
  mask.display_name = "Mask";
  RadialMaskSource radial;
  radial.center_x = center;
  mask.source     = std::move(radial);
  return mask;
}

auto DocumentWithGrades(int grade_count, int masks_per_grade) -> PipelineDocument {
  auto document = CreateDefaultPipelineDocument();
  auto fill     = [&](ColorGradeNodeModel* grade, const std::string& prefix) {
    if (grade == nullptr) {
      return;
    }
    for (int mask = 0; mask < masks_per_grade; ++mask) {
      grade->AddMask(MakeMask(prefix + "." + std::to_string(mask), 0.1f * static_cast<float>(mask)),
                     static_cast<std::size_t>(mask));
    }
  };
  fill(document.PrimaryGrade(), "mask.primary");
  for (int extra = 0; extra < grade_count - 1; ++extra) {
    const auto id = NodeId{"grade.g" + std::to_string(extra)};
    EXPECT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, id).empty());
    fill(dynamic_cast<ColorGradeNodeModel*>(document.Graph().FindNode(id)),
         "mask.g" + std::to_string(extra));
  }
  return document;
}

void ExpectNoWholeDraftCopy(const EditorNodeGraphDraft& draft) {
  EXPECT_EQ(draft.work_stats().complete_state_copies, 0);
  EXPECT_EQ(draft.work_stats().complete_index_rebuilds, 0);
}

TEST(EditorNodeGraphDraft, FirstConstructionCopiesTheCompleteProjectionOnce) {
  const auto document = CreateDefaultPipelineDocument();
  auto       draft    = EditorNodeGraphDraft::FromDocument(document, BoundIdentity());
  EXPECT_TRUE(draft.MatchesIdentity(BoundIdentity()));
  EXPECT_TRUE(draft.MatchesBase(document));
  EXPECT_EQ(draft.Nodes().size(), 3u);
  EXPECT_EQ(draft.Edges().size(), 2u);
  EXPECT_TRUE(draft.DeltaEmpty());
  EXPECT_TRUE(draft.SubmissionValid());
}

TEST(EditorNodeGraphDraft, AddInsertsOneDisconnectedGradeWithoutConsumingTheProductCounter) {
  auto document = CreateDefaultPipelineDocument();
  auto draft    = EditorNodeGraphDraft::FromDocument(document, BoundIdentity());
  const auto* ptr = &draft;
  auto        add = draft.AddColorGrade(NodeId{"grade.extra"});
  ASSERT_TRUE(add.succeeded);
  EXPECT_FALSE(add.submission_valid);
  EXPECT_FALSE(add.delta_empty);
  EXPECT_EQ(&draft, ptr);
  EXPECT_EQ(draft.Nodes().size(), 4u);
  EXPECT_EQ(draft.Edges().size(), 2u);
  EXPECT_EQ(draft.FindNode(NodeId{"grade.extra"})->display_name, "Color Grade 2");
  EXPECT_EQ(document.NextColorGradeNameNumber(), 2u);
  EXPECT_TRUE(draft.MatchesBase(document));
}

TEST(EditorNodeGraphDraft, ExclusivePortConnectReplacesOnlyTheRequestedPorts) {
  auto document = CreateDefaultPipelineDocument();
  auto draft    = EditorNodeGraphDraft::FromDocument(document, BoundIdentity());
  ASSERT_TRUE(draft.AddColorGrade(NodeId{"grade.d"}).succeeded);

  auto first = draft.Connect(NodeId{"develop"}, NodeId{"grade.d"});
  ASSERT_TRUE(first.succeeded);
  EXPECT_EQ(first.removed_edges.size(), 1u);
  EXPECT_EQ(first.inserted_edges.size(), 1u);
  EXPECT_FALSE(first.submission_valid);
  EXPECT_EQ(draft.Edges().size(), 2u);
  EXPECT_EQ(draft.Edges()[0].source_node_id, NodeId{"grade.primary"});
  EXPECT_EQ(draft.Edges()[0].destination_node_id, NodeId{"drt"});
  EXPECT_EQ(draft.Edges()[1].source_node_id, NodeId{"develop"});
  EXPECT_EQ(draft.Edges()[1].destination_node_id, NodeId{"grade.d"});

  auto second = draft.Connect(NodeId{"grade.d"}, NodeId{"drt"});
  ASSERT_TRUE(second.succeeded);
  EXPECT_FALSE(second.submission_valid);
  bool primary_on_path = false;
  for (const auto& edge : draft.Edges()) {
    if (edge.source_node_id == NodeId{"grade.primary"} ||
        edge.destination_node_id == NodeId{"grade.primary"}) {
      primary_on_path = true;
    }
  }
  EXPECT_FALSE(primary_on_path);
}

TEST(EditorNodeGraphDraft, ReconnectingTheDetachedGradeMakesTheDraftSubmittable) {
  auto document = CreateDefaultPipelineDocument();
  auto draft    = EditorNodeGraphDraft::FromDocument(document, BoundIdentity());
  ASSERT_TRUE(draft.AddColorGrade(NodeId{"grade.d"}).succeeded);
  ASSERT_TRUE(draft.Connect(NodeId{"develop"}, NodeId{"grade.d"}).succeeded);
  ASSERT_TRUE(draft.Connect(NodeId{"grade.d"}, NodeId{"drt"}).succeeded);
  ASSERT_FALSE(draft.SubmissionValid());

  auto reconnect = draft.Connect(NodeId{"grade.d"}, NodeId{"grade.primary"});
  ASSERT_TRUE(reconnect.succeeded);
  auto finish = draft.Connect(NodeId{"grade.primary"}, NodeId{"drt"});
  ASSERT_TRUE(finish.succeeded);
  EXPECT_TRUE(finish.submission_valid);
  EXPECT_FALSE(finish.delta_empty);
  const auto change = draft.MakeChange();
  EXPECT_EQ(change.inserted_nodes.size(), 1u);
  EXPECT_TRUE(change.removed_nodes.empty());
}

TEST(EditorNodeGraphDraft, DeletingTheDetachedGradeMakesThePathValid) {
  auto document = CreateDefaultPipelineDocument();
  auto draft    = EditorNodeGraphDraft::FromDocument(document, BoundIdentity());
  ASSERT_TRUE(draft.AddColorGrade(NodeId{"grade.d"}).succeeded);
  ASSERT_TRUE(draft.Connect(NodeId{"develop"}, NodeId{"grade.d"}).succeeded);
  ASSERT_TRUE(draft.Connect(NodeId{"grade.d"}, NodeId{"drt"}).succeeded);
  ASSERT_FALSE(draft.SubmissionValid());
  auto remove = draft.RemoveColorGrade(NodeId{"grade.primary"});
  ASSERT_TRUE(remove.succeeded);
  EXPECT_TRUE(remove.submission_valid);
  EXPECT_EQ(draft.FindNode(NodeId{"grade.primary"}), nullptr);
}

TEST(EditorNodeGraphDraft, ReturningToTheBaseEmptiesTheDelta) {
  auto document = CreateDefaultPipelineDocument();
  auto draft    = EditorNodeGraphDraft::FromDocument(document, BoundIdentity());
  ASSERT_TRUE(draft.AddColorGrade(NodeId{"grade.d"}).succeeded);
  auto remove = draft.RemoveColorGrade(NodeId{"grade.d"});
  ASSERT_TRUE(remove.succeeded);
  EXPECT_TRUE(remove.delta_empty);
  EXPECT_TRUE(draft.DeltaEmpty());
  EXPECT_TRUE(draft.SubmissionValid());
  EXPECT_EQ(draft.NextColorGradeNameNumber(), 2u);
}

TEST(EditorNodeGraphDraft, UnsupportedConnectLeavesValuesIndexesAndDeltaUnchanged) {
  auto document = CreateDefaultPipelineDocument();
  auto draft    = EditorNodeGraphDraft::FromDocument(document, BoundIdentity());
  ASSERT_TRUE(draft.AddColorGrade(NodeId{"grade.d"}).succeeded);
  const auto before_nodes = draft.Nodes();
  const auto before_edges = draft.Edges();
  const auto before_delta = draft.DeltaEmpty();

  auto self = draft.Connect(NodeId{"grade.d"}, NodeId{"grade.d"});
  EXPECT_FALSE(self.succeeded);
  EXPECT_EQ(self.issue, NodeGraphDraftIssue::SelfConnection);
  EXPECT_EQ(self.error, "A node cannot connect to itself");

  auto into_develop = draft.Connect(NodeId{"grade.d"}, NodeId{"develop"});
  EXPECT_FALSE(into_develop.succeeded);
  EXPECT_EQ(into_develop.issue, NodeGraphDraftIssue::DevelopHasNoIncomingPort);
  EXPECT_EQ(into_develop.error, "Develop has no incoming image port");

  auto from_drt = draft.Connect(NodeId{"drt"}, NodeId{"grade.d"});
  EXPECT_FALSE(from_drt.succeeded);
  EXPECT_EQ(from_drt.issue, NodeGraphDraftIssue::DrtHasNoOutgoingPort);
  EXPECT_EQ(from_drt.error, "DRT/Post has no outgoing image port");

  EXPECT_EQ(draft.Nodes().size(), before_nodes.size());
  EXPECT_EQ(draft.Edges().size(), before_edges.size());
  EXPECT_EQ(draft.DeltaEmpty(), before_delta);
}

TEST(EditorNodeGraphDraft, RestoreLastMutationReversesAnAdmittedConnect) {
  auto document = CreateDefaultPipelineDocument();
  auto draft    = EditorNodeGraphDraft::FromDocument(document, BoundIdentity());
  ASSERT_TRUE(draft.AddColorGrade(NodeId{"grade.d"}).succeeded);
  ASSERT_TRUE(draft.Connect(NodeId{"develop"}, NodeId{"grade.d"}).succeeded);
  EXPECT_TRUE(draft.HasLastMutation());
  draft.RestoreLastMutation();
  EXPECT_EQ(draft.Edges().size(), 2u);
  EXPECT_EQ(draft.Edges()[0].source_node_id, NodeId{"develop"});
  EXPECT_EQ(draft.Edges()[0].destination_node_id, NodeId{"grade.primary"});
}

TEST(EditorNodeGraphDraft, AddDeleteConnectReversalRestoresExactCounterOrderAndJson) {
  auto  document = CreateDefaultPipelineDocument();
  auto* grade    = document.PrimaryGrade();
  ASSERT_NE(grade, nullptr);
  grade->AddMask(MakeMask("mask.keep", 0.4f), 0);
  auto       draft       = EditorNodeGraphDraft::FromDocument(document, BoundIdentity());
  const auto json_before = *draft.NodeJson(NodeId{"grade.primary"});
  const auto nodes_before = draft.Nodes();
  const auto edges_before = draft.Edges();
  const auto counter      = draft.NextColorGradeNameNumber();
  draft.ResetWorkStats();

  ASSERT_TRUE(draft.AddColorGrade(NodeId{"grade.d"}).succeeded);
  ExpectNoWholeDraftCopy(draft);
  draft.RestoreLastMutation();
  EXPECT_EQ(draft.FindNode(NodeId{"grade.d"}), nullptr);
  EXPECT_EQ(draft.NextColorGradeNameNumber(), counter);
  EXPECT_EQ(draft.Nodes(), nodes_before);
  EXPECT_EQ(draft.Edges(), edges_before);

  ASSERT_TRUE(draft.AddColorGrade(NodeId{"grade.d"}).succeeded);
  ASSERT_TRUE(draft.Connect(NodeId{"develop"}, NodeId{"grade.d"}).succeeded);
  draft.RestoreLastMutation();
  EXPECT_EQ(draft.Edges()[0].source_node_id, NodeId{"develop"});
  EXPECT_EQ(draft.Edges()[0].destination_node_id, NodeId{"grade.primary"});
  EXPECT_NE(draft.FindNode(NodeId{"grade.d"}), nullptr);

  ASSERT_TRUE(draft.Connect(NodeId{"develop"}, NodeId{"grade.d"}).succeeded);
  ASSERT_TRUE(draft.RemoveColorGrade(NodeId{"grade.primary"}).succeeded);
  EXPECT_EQ(draft.FindNode(NodeId{"grade.primary"}), nullptr);
  draft.RestoreLastMutation();
  EXPECT_NE(draft.FindNode(NodeId{"grade.primary"}), nullptr);
  EXPECT_EQ(*draft.NodeJson(NodeId{"grade.primary"}), json_before);
  EXPECT_EQ(draft.FindNode(NodeId{"grade.d"})->display_name, "Color Grade 2");
  ExpectNoWholeDraftCopy(draft);
}

TEST(EditorNodeGraphDraft, NetCancellationRestoresBaseWithoutWholeDraftCopy) {
  auto document = CreateDefaultPipelineDocument();
  auto draft    = EditorNodeGraphDraft::FromDocument(document, BoundIdentity());
  draft.ResetWorkStats();
  ASSERT_TRUE(draft.AddColorGrade(NodeId{"grade.d"}).succeeded);
  auto remove = draft.RemoveColorGrade(NodeId{"grade.d"});
  ASSERT_TRUE(remove.succeeded);
  EXPECT_TRUE(remove.delta_empty);
  EXPECT_TRUE(draft.DeltaEmpty());
  EXPECT_TRUE(draft.SubmissionValid());
  EXPECT_EQ(draft.Nodes().size(), 3u);
  ExpectNoWholeDraftCopy(draft);
}

TEST(EditorNodeGraphDraft, MultipleDetachedGradesKeepIndependentNodesAndEdges) {
  auto document = CreateDefaultPipelineDocument();
  auto draft    = EditorNodeGraphDraft::FromDocument(document, BoundIdentity());
  draft.ResetWorkStats();
  ASSERT_TRUE(draft.AddColorGrade(NodeId{"grade.a"}).succeeded);
  ASSERT_TRUE(draft.AddColorGrade(NodeId{"grade.b"}).succeeded);
  EXPECT_EQ(draft.Nodes().size(), 5u);
  EXPECT_EQ(draft.Edges().size(), 2u);
  EXPECT_NE(draft.FindNode(NodeId{"grade.a"}), nullptr);
  EXPECT_NE(draft.FindNode(NodeId{"grade.b"}), nullptr);
  EXPECT_EQ(draft.FindNode(NodeId{"grade.a"})->display_name, "Color Grade 2");
  EXPECT_EQ(draft.FindNode(NodeId{"grade.b"})->display_name, "Color Grade 3");
  draft.RestoreLastMutation();
  EXPECT_EQ(draft.FindNode(NodeId{"grade.b"}), nullptr);
  EXPECT_NE(draft.FindNode(NodeId{"grade.a"}), nullptr);
  EXPECT_EQ(draft.Edges().size(), 2u);
  ExpectNoWholeDraftCopy(draft);
}

TEST(EditorNodeGraphDraft, RejectedConnectRetainsCachedSubmissionValidity) {
  auto document = CreateDefaultPipelineDocument();
  auto draft    = EditorNodeGraphDraft::FromDocument(document, BoundIdentity());
  draft.ResetWorkStats();
  ASSERT_TRUE(draft.AddColorGrade(NodeId{"grade.d"}).succeeded);
  EXPECT_FALSE(draft.SubmissionValid());
  const auto traversals = draft.work_stats().validity_traversals;
  EXPECT_GE(traversals, 1);
  auto self = draft.Connect(NodeId{"grade.d"}, NodeId{"grade.d"});
  EXPECT_FALSE(self.succeeded);
  EXPECT_EQ(draft.work_stats().validity_traversals, traversals);
  EXPECT_FALSE(draft.SubmissionValid());
}

TEST(EditorNodeGraphDraft, SerializedDraftChangeMatchesInPlaceForwardAndInverse) {
  auto document = CreateDefaultPipelineDocument();
  auto draft    = EditorNodeGraphDraft::FromDocument(document, BoundIdentity());
  ASSERT_TRUE(draft.AddColorGrade(NodeId{"grade.d"}).succeeded);
  ASSERT_TRUE(draft.Connect(NodeId{"develop"}, NodeId{"grade.d"}).succeeded);
  ASSERT_TRUE(draft.Connect(NodeId{"grade.d"}, NodeId{"grade.primary"}).succeeded);
  ASSERT_TRUE(draft.Connect(NodeId{"grade.primary"}, NodeId{"drt"}).succeeded);
  ASSERT_TRUE(draft.SubmissionValid());
  const auto change      = draft.MakeChange();
  const auto before_json = document.ToJson().dump();
  auto       forward     = ClonePipelineDocument(document);
  ASSERT_TRUE(ApplyNodeGraphTopologyChange(forward, change, PipelineEditApplyDirection::Forward)
                  .empty());
  EXPECT_NE(forward.Graph().FindNode(NodeId{"grade.d"}), nullptr);
  ASSERT_TRUE(ApplyNodeGraphTopologyChange(forward, change, PipelineEditApplyDirection::Inverse)
                  .empty());
  EXPECT_EQ(forward.ToJson().dump(), before_json);
  EXPECT_EQ(forward.NextColorGradeNameNumber(), document.NextColorGradeNameNumber());
}

TEST(EditorNodeGraphDraft, ThirtyTwoGradeGraphRepeatedConnectStaysBounded) {
  auto document = DocumentWithGrades(32, 8);
  EXPECT_EQ(document.Graph().NodeCount(), 34u);
  auto draft = EditorNodeGraphDraft::FromDocument(document, BoundIdentity());
  EXPECT_TRUE(draft.SubmissionValid());
  draft.ResetWorkStats();
  ASSERT_TRUE(draft.AddColorGrade(NodeId{"grade.extra"}).succeeded);
  const NodeId develop{"develop"};
  const NodeId extra{"grade.extra"};
  const NodeId primary{"grade.primary"};
  for (int step = 0; step < 50; ++step) {
    ASSERT_TRUE(draft.Connect(develop, extra).succeeded) << step;
    ASSERT_TRUE(draft.Connect(develop, primary).succeeded) << step;
  }
  ExpectNoWholeDraftCopy(draft);
  EXPECT_EQ(draft.work_stats().validity_traversals, 101);
  EXPECT_LT(draft.work_stats().node_entry_copies, 8);
  EXPECT_LT(draft.work_stats().edge_entry_copies, 400);
}

}  // namespace
