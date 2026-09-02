//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/graph/pipeline_graph_commands.hpp"

#include <cmath>
#include <string>
#include <utility>

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/port.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"

namespace alcedo {

const PortId kImagePort{"image"};

namespace {

auto Error(GraphValidationCode code, std::string message) -> GraphValidationError {
  return {code, std::move(message)};
}

/// Mark topology only after the graph accepted and completed a local edit.
auto FinishEdit(PipelineDocument& document, std::vector<GraphValidationError> errors)
    -> std::vector<GraphValidationError> {
  if (errors.empty()) document.MarkTopologyDirty();
  return errors;
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

}  // namespace

auto FindSceneImagePredecessor(const PipelineGraph& graph, const NodeId& node_id)
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

auto FindSceneImageSuccessor(const PipelineGraph& graph, const NodeId& node_id)
    -> const GraphEdge* {
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

auto AddCleanColorGrade(PipelineDocument& document, const NodeId& before_node_id, NodeId new_id)
    -> std::vector<GraphValidationError> {
  if (new_id.Empty()) {
    return {Error(GraphValidationCode::UnknownNode, "AddCleanColorGrade requires a NodeId")};
  }
  if (document.Graph().FindNode(new_id) != nullptr) {
    return {Error(GraphValidationCode::DuplicateNodeId,
                  "Duplicate node id: " + std::string{new_id.Value()})};
  }
  const auto* before = document.Graph().FindNode(before_node_id);
  if (before == nullptr) {
    return {Error(GraphValidationCode::UnknownNode,
                  "Unknown insert point: " + std::string{before_node_id.Value()})};
  }
  if (before->Type() == type_ids::DevelopNode()) {
    return {Error(GraphValidationCode::ProtectedEndpoint, "Cannot insert a Color Grade before Develop")};
  }
  const auto* incoming = FindSceneImagePredecessor(document.Graph(), before_node_id);
  if (incoming == nullptr) {
    return {Error(GraphValidationCode::BrokenImageBackbone,
                  "Insert point has no scene-image predecessor: " +
                      std::string{before_node_id.Value()})};
  }

  const GraphEdge previous = *incoming;
  auto            node     = CreateCleanColorGradeNode(new_id);
  return FinishEdit(document, document.Graph().ApplyBackboneEdit(
                                  {previous},
                                  {{previous.from_node, previous.from_port, new_id, kImagePort},
                                   {new_id, kImagePort, before_node_id, previous.to_port}},
                                  std::move(node)));
}

auto RemoveColorGradeAndBridge(PipelineDocument& document, const NodeId& node_id)
    -> std::vector<GraphValidationError> {
  auto errors = RequireColorGrade(document, node_id);
  if (!errors.empty()) {
    return errors;
  }
  const auto* incoming = FindSceneImagePredecessor(document.Graph(), node_id);
  const auto* outgoing = FindSceneImageSuccessor(document.Graph(), node_id);
  if (incoming == nullptr || outgoing == nullptr) {
    return {Error(GraphValidationCode::BrokenImageBackbone,
                  "Color Grade is not on a scene-image edge pair: " +
                      std::string{node_id.Value()})};
  }

  return FinishEdit(
      document,
      document.Graph().ApplyBackboneEdit(
          {}, {{incoming->from_node, incoming->from_port, outgoing->to_node, outgoing->to_port}},
          nullptr, node_id));
}

auto ReconnectColorGrade(PipelineDocument& document, const NodeId& node_id,
                         const NodeId& new_predecessor_id, const NodeId& new_successor_id)
    -> std::vector<GraphValidationError> {
  auto errors = RequireColorGrade(document, node_id);
  if (!errors.empty()) {
    return errors;
  }
  if (node_id == new_predecessor_id || node_id == new_successor_id ||
      new_predecessor_id == new_successor_id) {
    return {Error(GraphValidationCode::BrokenImageBackbone,
                  "Reconnect requires distinct node, predecessor, and successor")};
  }
  if (document.Graph().FindNode(new_predecessor_id) == nullptr) {
    return {Error(GraphValidationCode::UnknownNode,
                  "Unknown predecessor: " + std::string{new_predecessor_id.Value()})};
  }
  if (document.Graph().FindNode(new_successor_id) == nullptr) {
    return {Error(GraphValidationCode::UnknownNode,
                  "Unknown successor: " + std::string{new_successor_id.Value()})};
  }
  const auto* incoming = FindSceneImagePredecessor(document.Graph(), node_id);
  const auto* outgoing = FindSceneImageSuccessor(document.Graph(), node_id);
  if (incoming == nullptr || outgoing == nullptr) {
    return {Error(GraphValidationCode::BrokenImageBackbone,
                  "Color Grade is not on a scene-image edge pair: " +
                      std::string{node_id.Value()})};
  }

  // Moving to the current location changes neither topology nor ownership.
  if (incoming->from_node == new_predecessor_id && outgoing->to_node == new_successor_id) {
    return document.Graph().ApplyBackboneEdit({}, {});
  }
  const auto* insert_edge =
      FindSceneImageEdge(document.Graph(), new_predecessor_id, new_successor_id);
  std::vector<GraphEdge> disconnected{*incoming, *outgoing};
  if (insert_edge != nullptr) disconnected.push_back(*insert_edge);
  return FinishEdit(document,
                    document.Graph().ApplyBackboneEdit(
                        disconnected, {{incoming->from_node, incoming->from_port, outgoing->to_node,
                                        outgoing->to_port},
                                       {new_predecessor_id, kImagePort, node_id, kImagePort},
                                       {node_id, kImagePort, new_successor_id, kImagePort}}));
}

auto RenameColorGrade(PipelineDocument& document, const NodeId& node_id, std::string display_name)
    -> std::vector<GraphValidationError> {
  auto errors = RequireColorGrade(document, node_id);
  if (!errors.empty()) {
    return errors;
  }
  if (display_name.empty()) {
    return {Error(GraphValidationCode::InvalidDisplayName, "Color Grade display name cannot be empty")};
  }
  auto* grade = dynamic_cast<ColorGradeNodeModel*>(document.Graph().FindNode(node_id));
  if (grade == nullptr) {
    return {Error(GraphValidationCode::NotAColorGrade,
                  "Node is not a Color Grade: " + std::string{node_id.Value()})};
  }
  grade->SetDisplayName(std::move(display_name));
  return {};
}

auto SetColorGradeEnabled(PipelineDocument& document, const NodeId& node_id, bool enabled)
    -> std::vector<GraphValidationError> {
  auto errors = RequireColorGrade(document, node_id);
  if (!errors.empty()) {
    return errors;
  }
  auto* grade = dynamic_cast<ColorGradeNodeModel*>(document.Graph().FindNode(node_id));
  if (grade == nullptr) {
    return {Error(GraphValidationCode::NotAColorGrade,
                  "Node is not a Color Grade: " + std::string{node_id.Value()})};
  }
  grade->SetEnabled(enabled);
  return {};
}

auto SetColorGradeMix(PipelineDocument& document, const NodeId& node_id, float mix)
    -> std::vector<GraphValidationError> {
  auto errors = RequireColorGrade(document, node_id);
  if (!errors.empty()) {
    return errors;
  }
  if (!std::isfinite(mix) || mix < 0.0f || mix > 1.0f) {
    return {Error(GraphValidationCode::InvalidNodeValue, "Color Grade mix must stay in [0, 1]")};
  }
  auto* grade = dynamic_cast<ColorGradeNodeModel*>(document.Graph().FindNode(node_id));
  if (grade == nullptr) {
    return {Error(GraphValidationCode::NotAColorGrade,
                  "Node is not a Color Grade: " + std::string{node_id.Value()})};
  }
  grade->SetMix(mix);
  return {};
}

auto InsertColorGradeFromJson(PipelineDocument& document, nlohmann::json node_json,
                              const GraphEdge& incoming, const GraphEdge& outgoing)
    -> std::vector<GraphValidationError> {
  std::unique_ptr<ColorGradeNodeModel> node;
  try {
    node = ColorGradeNodeModel::FromJson(node_json);
  } catch (const std::exception& ex) {
    return {Error(GraphValidationCode::UnknownNode,
                  std::string{"InsertColorGradeFromJson: "} + ex.what())};
  }
  if (node == nullptr) {
    return {Error(GraphValidationCode::UnknownNode, "InsertColorGradeFromJson requires a node")};
  }
  if (incoming.to_node != node->Id() || outgoing.from_node != node->Id()) {
    return {Error(GraphValidationCode::BrokenImageBackbone,
                  "InsertColorGradeFromJson edges must attach to the stored node")};
  }
  if (document.Graph().FindNode(node->Id()) != nullptr) {
    return {Error(GraphValidationCode::DuplicateNodeId,
                  "Duplicate node id: " + std::string{node->Id().Value()})};
  }
  const GraphEdge previous{incoming.from_node, incoming.from_port, outgoing.to_node,
                           outgoing.to_port};
  return FinishEdit(document, document.Graph().ApplyBackboneEdit(
                                  {previous}, {incoming, outgoing}, std::move(node)));
}

}  // namespace alcedo
