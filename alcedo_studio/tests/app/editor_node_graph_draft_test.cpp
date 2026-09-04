//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include "app/editor_node_graph_draft.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_commands.hpp"

namespace {

using alcedo::CreateDefaultPipelineDocument;
using alcedo::EditorNodeGraphDraft;
using alcedo::EditorNodeGraphDraftIdentity;
using alcedo::NodeId;

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
  EXPECT_EQ(self.error, "A node cannot connect to itself");

  auto into_develop = draft.Connect(NodeId{"grade.d"}, NodeId{"develop"});
  EXPECT_FALSE(into_develop.succeeded);
  EXPECT_EQ(into_develop.error, "Develop has no incoming image port");

  auto from_drt = draft.Connect(NodeId{"drt"}, NodeId{"grade.d"});
  EXPECT_FALSE(from_drt.succeeded);
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

}  // namespace
