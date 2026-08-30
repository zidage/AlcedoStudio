//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/graph/pipeline_graph_commands.hpp"

#include <string>
#include <utility>

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/port.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "json.hpp"

namespace alcedo {
namespace {

const PortId kImagePort{"image"};

auto Error(GraphValidationCode code, std::string message) -> GraphValidationError {
  return {code, std::move(message)};
}

auto CombineValidation(const PipelineDocument& document) -> std::vector<GraphValidationError> {
  auto errors = document.Graph().Validate();
  auto backbone = document.Graph().ValidateImageBackbone();
  errors.insert(errors.end(), backbone.begin(), backbone.end());
  return errors;
}

auto Restore(PipelineDocument& candidate, const nlohmann::json& before) -> void {
  candidate = PipelineDocument::FromJson(before);
}

auto CommitOrRestore(PipelineDocument& candidate, const nlohmann::json& before)
    -> std::vector<GraphValidationError> {
  auto errors = CombineValidation(candidate);
  if (!errors.empty()) {
    Restore(candidate, before);
    return errors;
  }
  candidate.MarkTopologyDirty();
  return {};
}

auto RequireColorGrade(const PipelineDocument& document, const NodeId& node_id)
    -> std::vector<GraphValidationError> {
  const auto* node = document.Graph().FindNode(node_id);
  if (node == nullptr) {
    return {Error(GraphValidationCode::UnknownNode,
                  "Unknown node: " + std::string{node_id.Value()})};
  }
  if (node->Type() == type_ids::DevelopNode() || node->Type() == type_ids::DrtNode()) {
    return {Error(GraphValidationCode::ProtectedEndpoint,
                  "Develop and DRT cannot be edited as Color Grades: " +
                      std::string{node_id.Value()})};
  }
  if (node->Type() != type_ids::ColorGradeNode()) {
    return {Error(GraphValidationCode::NotAColorGrade,
                  "Node is not a Color Grade: " + std::string{node_id.Value()})};
  }
  return {};
}

auto SceneImagePredecessor(const PipelineGraph& graph, const NodeId& node_id)
    -> const GraphEdge* {
  const auto* node = graph.FindNode(node_id);
  if (node == nullptr) {
    return nullptr;
  }
  const GraphEdge* found = nullptr;
  for (const auto& edge : graph.Edges()) {
    if (edge.to_node != node_id) {
      continue;
    }
    const auto* to_port = [&]() -> const PortDescriptor* {
      for (const auto& port : node->InputPorts()) {
        if (port.id == edge.to_port) {
          return &port;
        }
      }
      return nullptr;
    }();
    if (to_port == nullptr || to_port->data_type != PortDataType::SceneImage) {
      continue;
    }
    if (found != nullptr) {
      return nullptr;
    }
    found = &edge;
  }
  return found;
}

auto SceneImageSuccessor(const PipelineGraph& graph, const NodeId& node_id) -> const GraphEdge* {
  const auto* node = graph.FindNode(node_id);
  if (node == nullptr) {
    return nullptr;
  }
  const GraphEdge* found = nullptr;
  for (const auto& edge : graph.Edges()) {
    if (edge.from_node != node_id) {
      continue;
    }
    const auto* from_port = [&]() -> const PortDescriptor* {
      for (const auto& port : node->OutputPorts()) {
        if (port.id == edge.from_port) {
          return &port;
        }
      }
      return nullptr;
    }();
    if (from_port == nullptr || from_port->data_type != PortDataType::SceneImage) {
      continue;
    }
    if (found != nullptr) {
      return nullptr;
    }
    found = &edge;
  }
  return found;
}

auto FindSceneImageEdge(const PipelineGraph& graph, const NodeId& from, const NodeId& to)
    -> const GraphEdge* {
  for (const auto& edge : graph.Edges()) {
    if (edge.from_node == from && edge.to_node == to && edge.from_port == kImagePort &&
        edge.to_port == kImagePort) {
      return &edge;
    }
  }
  return nullptr;
}

void BridgeSceneImage(PipelineGraph& graph, const NodeId& predecessor, const NodeId& successor) {
  graph.Connect(predecessor, kImagePort, successor, kImagePort);
}

void SpliceOutColorGrade(PipelineGraph& graph, const NodeId& node_id) {
  const auto* incoming = SceneImagePredecessor(graph, node_id);
  const auto* outgoing  = SceneImageSuccessor(graph, node_id);
  NodeId predecessor;
  NodeId successor;
  PortId in_from_port;
  PortId in_to_port;
  PortId out_from_port;
  PortId out_to_port;
  const bool has_in  = incoming != nullptr;
  const bool has_out = outgoing != nullptr;
  if (has_in) {
    predecessor = incoming->from_node;
    in_from_port = incoming->from_port;
    in_to_port   = incoming->to_port;
  }
  if (has_out) {
    successor     = outgoing->to_node;
    out_from_port = outgoing->from_port;
    out_to_port   = outgoing->to_port;
  }
  if (has_in) {
    graph.Disconnect(predecessor, in_from_port, node_id, in_to_port);
  }
  if (has_out) {
    graph.Disconnect(node_id, out_from_port, successor, out_to_port);
  }
  if (has_in && has_out) {
    BridgeSceneImage(graph, predecessor, successor);
  }
}

}  // namespace

auto AddCleanColorGrade(PipelineDocument& candidate, const NodeId& before_node_id, NodeId new_id)
    -> std::vector<GraphValidationError> {
  if (new_id.Empty()) {
    return {Error(GraphValidationCode::UnknownNode, "AddCleanColorGrade requires a NodeId")};
  }
  if (candidate.Graph().FindNode(new_id) != nullptr) {
    return {Error(GraphValidationCode::DuplicateNodeId,
                  "Duplicate node id: " + std::string{new_id.Value()})};
  }
  const auto* before = candidate.Graph().FindNode(before_node_id);
  if (before == nullptr) {
    return {Error(GraphValidationCode::UnknownNode,
                  "Unknown insert point: " + std::string{before_node_id.Value()})};
  }
  if (before->Type() == type_ids::DevelopNode()) {
    return {Error(GraphValidationCode::ProtectedEndpoint, "Cannot insert a Color Grade before Develop")};
  }
  const auto* incoming = SceneImagePredecessor(candidate.Graph(), before_node_id);
  if (incoming == nullptr) {
    return {Error(GraphValidationCode::BrokenImageBackbone,
                  "Insert point has no scene-image predecessor: " +
                      std::string{before_node_id.Value()})};
  }

  const auto before_json = candidate.ToJson();
  const NodeId predecessor = incoming->from_node;
  const PortId from_port  = incoming->from_port;
  const PortId to_port    = incoming->to_port;
  const NodeId inserted_id = new_id;
  candidate.Graph().Disconnect(predecessor, from_port, before_node_id, to_port);
  candidate.Graph().AddNode(CreateCleanColorGradeNode(inserted_id));
  candidate.Graph().Connect(predecessor, kImagePort, inserted_id, kImagePort);
  candidate.Graph().Connect(inserted_id, kImagePort, before_node_id, kImagePort);
  return CommitOrRestore(candidate, before_json);
}

auto RemoveColorGradeAndBridge(PipelineDocument& candidate, const NodeId& node_id)
    -> std::vector<GraphValidationError> {
  auto errors = RequireColorGrade(candidate, node_id);
  if (!errors.empty()) {
    return errors;
  }
  const auto* incoming = SceneImagePredecessor(candidate.Graph(), node_id);
  const auto* outgoing = SceneImageSuccessor(candidate.Graph(), node_id);
  if (incoming == nullptr || outgoing == nullptr) {
    return {Error(GraphValidationCode::BrokenImageBackbone,
                  "Color Grade is not on a scene-image edge pair: " +
                      std::string{node_id.Value()})};
  }

  const auto before_json = candidate.ToJson();
  SpliceOutColorGrade(candidate.Graph(), node_id);
  candidate.Graph().RemoveNode(node_id);
  return CommitOrRestore(candidate, before_json);
}

auto ReconnectColorGrade(PipelineDocument& candidate, const NodeId& node_id,
                          const NodeId& new_predecessor_id, const NodeId& new_successor_id)
    -> std::vector<GraphValidationError> {
  auto errors = RequireColorGrade(candidate, node_id);
  if (!errors.empty()) {
    return errors;
  }
  if (node_id == new_predecessor_id || node_id == new_successor_id ||
      new_predecessor_id == new_successor_id) {
    return {Error(GraphValidationCode::BrokenImageBackbone,
                  "Reconnect requires distinct node, predecessor, and successor")};
  }
  if (candidate.Graph().FindNode(new_predecessor_id) == nullptr) {
    return {Error(GraphValidationCode::UnknownNode,
                  "Unknown predecessor: " + std::string{new_predecessor_id.Value()})};
  }
  if (candidate.Graph().FindNode(new_successor_id) == nullptr) {
    return {Error(GraphValidationCode::UnknownNode,
                  "Unknown successor: " + std::string{new_successor_id.Value()})};
  }
  const auto* incoming = SceneImagePredecessor(candidate.Graph(), node_id);
  const auto* outgoing = SceneImageSuccessor(candidate.Graph(), node_id);
  if (incoming == nullptr || outgoing == nullptr) {
    return {Error(GraphValidationCode::BrokenImageBackbone,
                  "Color Grade is not on a scene-image edge pair: " +
                      std::string{node_id.Value()})};
  }

  const auto before_json = candidate.ToJson();
  SpliceOutColorGrade(candidate.Graph(), node_id);
  const auto* insert_edge =
      FindSceneImageEdge(candidate.Graph(), new_predecessor_id, new_successor_id);
  if (insert_edge != nullptr) {
    const NodeId from_node = insert_edge->from_node;
    const PortId from_port = insert_edge->from_port;
    const NodeId to_node   = insert_edge->to_node;
    const PortId to_port    = insert_edge->to_port;
    candidate.Graph().Disconnect(from_node, from_port, to_node, to_port);
  }
  candidate.Graph().Connect(new_predecessor_id, kImagePort, node_id, kImagePort);
  candidate.Graph().Connect(node_id, kImagePort, new_successor_id, kImagePort);
  return CommitOrRestore(candidate, before_json);
}

auto RenameColorGrade(PipelineDocument& candidate, const NodeId& node_id,
                        std::string display_name) -> std::vector<GraphValidationError> {
  auto errors = RequireColorGrade(candidate, node_id);
  if (!errors.empty()) {
    return errors;
  }
  if (display_name.empty()) {
    return {Error(GraphValidationCode::InvalidDisplayName, "Color Grade display name cannot be empty")};
  }
  auto* grade = dynamic_cast<ColorGradeNodeModel*>(candidate.Graph().FindNode(node_id));
  if (grade == nullptr) {
    return {Error(GraphValidationCode::NotAColorGrade,
                  "Node is not a Color Grade: " + std::string{node_id.Value()})};
  }
  const auto before_json = candidate.ToJson();
  grade->SetDisplayName(std::move(display_name));
  errors = CombineValidation(candidate);
  if (!errors.empty()) {
    Restore(candidate, before_json);
    return errors;
  }
  return {};
}

auto SetColorGradeEnabled(PipelineDocument& candidate, const NodeId& node_id, bool enabled)
    -> std::vector<GraphValidationError> {
  auto errors = RequireColorGrade(candidate, node_id);
  if (!errors.empty()) {
    return errors;
  }
  auto* grade = dynamic_cast<ColorGradeNodeModel*>(candidate.Graph().FindNode(node_id));
  if (grade == nullptr) {
    return {Error(GraphValidationCode::NotAColorGrade,
                  "Node is not a Color Grade: " + std::string{node_id.Value()})};
  }
  const auto before_json = candidate.ToJson();
  grade->SetEnabled(enabled);
  errors = CombineValidation(candidate);
  if (!errors.empty()) {
    Restore(candidate, before_json);
    return errors;
  }
  return {};
}

}  // namespace alcedo
