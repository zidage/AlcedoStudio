//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

#include "app/pipeline_document_history.hpp"
#include "app/pipeline_history_applier.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph.hpp"
#include "edit/graph/pipeline_graph_commands.hpp"

namespace {

using alcedo::ApplyNodeGraphTopologyChange;
using alcedo::ApplyPipelineEditBatch;
using alcedo::ColorGradeNodeModel;
using alcedo::CreateCleanColorGradeNode;
using alcedo::CreateDefaultPipelineDocument;
using alcedo::GraphEdge;
using alcedo::MakeEditNodeGraphBatch;
using alcedo::NodeGraphConnectedEdge;
using alcedo::NodeGraphDisconnectedEdge;
using alcedo::NodeGraphInsertedNode;
using alcedo::NodeGraphRemovedNode;
using alcedo::NodeGraphTopologyChange;
using alcedo::NodeId;
using alcedo::PipelineDocument;
using alcedo::PipelineEditApplyDirection;
using alcedo::PipelineSceneEdge;
using alcedo::PortId;
using alcedo::TopologyEdgeInsertion;
using alcedo::TopologyEdgeRemoval;
using alcedo::TopologyNodeInsertion;
using alcedo::TopologyNodeRemoval;

auto Image() -> PortId { return PortId{"image"}; }

auto Scene(const NodeId& from, const NodeId& to) -> PipelineSceneEdge {
  return PipelineSceneEdge{from, Image(), to, Image()};
}

auto NodeIndex(const PipelineDocument& document, const NodeId& id) -> std::size_t {
  for (std::size_t index = 0; index < document.Graph().Nodes().size(); ++index) {
    if (document.Graph().Nodes()[index]->Id() == id) {
      return index;
    }
  }
  ADD_FAILURE() << "missing node " << id.Value();
  return 0;
}

auto EdgeIndex(const PipelineDocument& document, const NodeId& from, const NodeId& to)
    -> std::size_t {
  const auto& edges = document.Graph().Edges();
  for (std::size_t index = 0; index < edges.size(); ++index) {
    if (edges[index].from_node == from && edges[index].to_node == to) {
      return index;
    }
  }
  ADD_FAILURE() << "missing edge";
  return 0;
}

auto InsertGradeChange(const PipelineDocument& document, NodeId id) -> NodeGraphTopologyChange {
  auto       node = CreateCleanColorGradeNode(id);
  node->SetDisplayName("Color Grade 2");
  NodeGraphTopologyChange change;
  change.before_next_color_grade_name_number = document.NextColorGradeNameNumber();
  change.after_next_color_grade_name_number  = change.before_next_color_grade_name_number + 1;
  NodeGraphInsertedNode inserted;
  inserted.node             = node->ToJson();
  inserted.final_node_index = static_cast<std::uint32_t>(document.Graph().NodeCount());
  change.inserted_nodes.push_back(std::move(inserted));
  return change;
}

TEST(PipelineGraphTopologyDelta, ForwardInsertKeepsUnaffectedNodeAddresses) {
  auto        document = CreateDefaultPipelineDocument();
  const auto* develop  = document.Graph().FindNode(NodeId{"develop"});
  const auto* primary  = document.Graph().FindNode(NodeId{"grade.primary"});
  const auto* drt      = document.Graph().FindNode(NodeId{"drt"});
  ASSERT_NE(develop, nullptr);
  ASSERT_NE(primary, nullptr);
  ASSERT_NE(drt, nullptr);

  auto change = InsertGradeChange(document, NodeId{"grade.extra"});
  // Incomplete graph: extra node with no edges. ApplyTopologyDelta validates the
  // complete graph, so connect Develop->primary->extra->DRT in one delta.
  change.disconnected_edges.push_back(
      NodeGraphDisconnectedEdge{Scene(NodeId{"grade.primary"}, NodeId{"drt"}),
                                static_cast<std::uint32_t>(EdgeIndex(
                                    document, NodeId{"grade.primary"}, NodeId{"drt"}))});
  change.connected_edges.push_back(NodeGraphConnectedEdge{
      Scene(NodeId{"grade.primary"}, NodeId{"grade.extra"}), 1});
  change.connected_edges.push_back(
      NodeGraphConnectedEdge{Scene(NodeId{"grade.extra"}, NodeId{"drt"}), 2});

  EXPECT_TRUE(ApplyNodeGraphTopologyChange(document, change, PipelineEditApplyDirection::Forward)
                  .empty());
  EXPECT_EQ(document.Graph().FindNode(NodeId{"develop"}), develop);
  EXPECT_EQ(document.Graph().FindNode(NodeId{"grade.primary"}), primary);
  EXPECT_EQ(document.Graph().FindNode(NodeId{"drt"}), drt);
  EXPECT_NE(document.Graph().FindNode(NodeId{"grade.extra"}), nullptr);
  EXPECT_EQ(document.NextColorGradeNameNumber(), 3u);
}

TEST(PipelineGraphTopologyDelta, InverseRestoresNodeOrderEdgesCounterAndObjectIdentity) {
  auto        document = CreateDefaultPipelineDocument();
  const auto* develop  = document.Graph().FindNode(NodeId{"develop"});
  const auto* primary  = document.Graph().FindNode(NodeId{"grade.primary"});
  const auto* drt      = document.Graph().FindNode(NodeId{"drt"});
  auto        change   = InsertGradeChange(document, NodeId{"grade.extra"});
  change.disconnected_edges.push_back(
      NodeGraphDisconnectedEdge{Scene(NodeId{"grade.primary"}, NodeId{"drt"}),
                                static_cast<std::uint32_t>(EdgeIndex(
                                    document, NodeId{"grade.primary"}, NodeId{"drt"}))});
  change.connected_edges.push_back(
      NodeGraphConnectedEdge{Scene(NodeId{"grade.primary"}, NodeId{"grade.extra"}), 1});
  change.connected_edges.push_back(
      NodeGraphConnectedEdge{Scene(NodeId{"grade.extra"}, NodeId{"drt"}), 2});
  ASSERT_TRUE(ApplyNodeGraphTopologyChange(document, change, PipelineEditApplyDirection::Forward)
                  .empty());

  EXPECT_TRUE(ApplyNodeGraphTopologyChange(document, change, PipelineEditApplyDirection::Inverse)
                  .empty());
  EXPECT_EQ(document.Graph().FindNode(NodeId{"develop"}), develop);
  EXPECT_EQ(document.Graph().FindNode(NodeId{"grade.primary"}), primary);
  EXPECT_EQ(document.Graph().FindNode(NodeId{"drt"}), drt);
  EXPECT_EQ(document.Graph().FindNode(NodeId{"grade.extra"}), nullptr);
  EXPECT_EQ(document.Graph().NodeCount(), 3u);
  EXPECT_EQ(document.Graph().Edges().size(), 2u);
  EXPECT_EQ(document.NextColorGradeNameNumber(), 2u);
  EXPECT_EQ(NodeIndex(document, NodeId{"develop"}), 0u);
  EXPECT_EQ(NodeIndex(document, NodeId{"grade.primary"}), 1u);
  EXPECT_EQ(NodeIndex(document, NodeId{"drt"}), 2u);
}

TEST(PipelineGraphTopologyDelta, ValidationFailureRestoresLiveGraphAndCounter) {
  auto document = CreateDefaultPipelineDocument();
  const auto* primary = document.Graph().FindNode(NodeId{"grade.primary"});
  auto        change  = InsertGradeChange(document, NodeId{"grade.extra"});
  // Insert the node without connecting it: backbone validation must fail.
  const auto before_json = document.ToJson().dump();
  const auto errors =
      ApplyNodeGraphTopologyChange(document, change, PipelineEditApplyDirection::Forward);
  EXPECT_FALSE(errors.empty());
  EXPECT_EQ(document.Graph().FindNode(NodeId{"grade.primary"}), primary);
  EXPECT_EQ(document.Graph().FindNode(NodeId{"grade.extra"}), nullptr);
  EXPECT_EQ(document.NextColorGradeNameNumber(), 2u);
  EXPECT_EQ(document.ToJson().dump(), before_json);
}

TEST(PipelineGraphTopologyDelta, FailureAfterEachStepRestoresExactState) {
  auto make_change = [](const PipelineDocument& document) {
    auto change = InsertGradeChange(document, NodeId{"grade.extra"});
    change.disconnected_edges.push_back(
        NodeGraphDisconnectedEdge{Scene(NodeId{"grade.primary"}, NodeId{"drt"}),
                                  static_cast<std::uint32_t>(EdgeIndex(
                                      document, NodeId{"grade.primary"}, NodeId{"drt"}))});
    change.connected_edges.push_back(
        NodeGraphConnectedEdge{Scene(NodeId{"grade.primary"}, NodeId{"grade.extra"}), 1});
    change.connected_edges.push_back(
        NodeGraphConnectedEdge{Scene(NodeId{"grade.extra"}, NodeId{"drt"}), 2});
    return change;
  };
  const std::vector<std::string> steps = {"counter", "disconnect", "insert_node", "connect",
                                          "validate"};
  for (const auto& fail_step : steps) {
    auto              document = CreateDefaultPipelineDocument();
    const auto*       develop  = document.Graph().FindNode(NodeId{"develop"});
    const auto*       primary  = document.Graph().FindNode(NodeId{"grade.primary"});
    const auto*       drt      = document.Graph().FindNode(NodeId{"drt"});
    const auto        before   = document.ToJson().dump();
    const auto        change   = make_change(document);
    try {
      (void)ApplyNodeGraphTopologyChange(
          document, change, PipelineEditApplyDirection::Forward,
          [&](std::string_view step, std::size_t) {
            if (step == fail_step) {
              throw std::runtime_error("injected " + std::string{step});
            }
          });
      FAIL() << "expected injected failure at " << fail_step;
    } catch (const std::runtime_error& ex) {
      EXPECT_EQ(std::string{ex.what()}, "injected " + fail_step);
    }
    EXPECT_EQ(document.Graph().FindNode(NodeId{"develop"}), develop) << fail_step;
    EXPECT_EQ(document.Graph().FindNode(NodeId{"grade.primary"}), primary) << fail_step;
    EXPECT_EQ(document.Graph().FindNode(NodeId{"drt"}), drt) << fail_step;
    EXPECT_EQ(document.Graph().FindNode(NodeId{"grade.extra"}), nullptr) << fail_step;
    EXPECT_EQ(document.NextColorGradeNameNumber(), 2u) << fail_step;
    EXPECT_EQ(document.ToJson().dump(), before) << fail_step;
  }
}

TEST(PipelineGraphTopologyDelta, TypedBatchApplyDoesNotCloneTheLiveGraph) {
  auto        document = CreateDefaultPipelineDocument();
  const auto* develop  = document.Graph().FindNode(NodeId{"develop"});
  const auto* primary  = document.Graph().FindNode(NodeId{"grade.primary"});
  const auto* drt      = document.Graph().FindNode(NodeId{"drt"});
  auto        change   = InsertGradeChange(document, NodeId{"grade.extra"});
  change.disconnected_edges.push_back(
      NodeGraphDisconnectedEdge{Scene(NodeId{"grade.primary"}, NodeId{"drt"}),
                                static_cast<std::uint32_t>(EdgeIndex(
                                    document, NodeId{"grade.primary"}, NodeId{"drt"}))});
  change.connected_edges.push_back(
      NodeGraphConnectedEdge{Scene(NodeId{"grade.primary"}, NodeId{"grade.extra"}), 1});
  change.connected_edges.push_back(
      NodeGraphConnectedEdge{Scene(NodeId{"grade.extra"}, NodeId{"drt"}), 2});
  std::string error;
  ASSERT_TRUE(ApplyPipelineEditBatch(document, MakeEditNodeGraphBatch(change),
                                     PipelineEditApplyDirection::Forward, &error))
      << error;
  EXPECT_EQ(document.Graph().FindNode(NodeId{"develop"}), develop);
  EXPECT_EQ(document.Graph().FindNode(NodeId{"grade.primary"}), primary);
  EXPECT_EQ(document.Graph().FindNode(NodeId{"drt"}), drt);
  ASSERT_TRUE(ApplyPipelineEditBatch(document, MakeEditNodeGraphBatch(change),
                                     PipelineEditApplyDirection::Inverse, &error))
      << error;
  EXPECT_EQ(document.Graph().FindNode(NodeId{"develop"}), develop);
  EXPECT_EQ(document.Graph().FindNode(NodeId{"grade.primary"}), primary);
  EXPECT_EQ(document.Graph().FindNode(NodeId{"drt"}), drt);
  EXPECT_EQ(document.Graph().FindNode(NodeId{"grade.extra"}), nullptr);
}

}  // namespace
