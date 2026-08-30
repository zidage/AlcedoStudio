//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/graph/pipeline_graph.hpp"

#include <algorithm>
#include <queue>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "edit/operators/models/builtin_type_ids.hpp"

namespace alcedo {

void PipelineGraph::AddNode(std::unique_ptr<INodeModel> node) {
  if (node == nullptr) {
    throw std::invalid_argument("AddNode requires a node");
  }
  if (FindNode(node->Id()) != nullptr) {
    throw std::invalid_argument("Duplicate node id: " + std::string{node->Id().Value()});
  }
  nodes_.push_back(std::move(node));
}

void PipelineGraph::Connect(NodeId from_node, PortId from_port, NodeId to_node, PortId to_port) {
  edges_.push_back(GraphEdge{std::move(from_node), std::move(from_port), std::move(to_node),
                             std::move(to_port)});
}

void PipelineGraph::Disconnect(const NodeId& from_node, const PortId& from_port,
                                 const NodeId& to_node, const PortId& to_port) {
  const auto it = std::find_if(edges_.begin(), edges_.end(), [&](const GraphEdge& edge) {
    return edge.from_node == from_node && edge.from_port == from_port && edge.to_node == to_node &&
           edge.to_port == to_port;
  });
  if (it != edges_.end()) {
    edges_.erase(it);
  }
}

void PipelineGraph::RemoveNode(const NodeId& id) {
  if (FindNode(id) == nullptr) {
    throw std::invalid_argument("Unknown node: " + std::string{id.Value()});
  }
  edges_.erase(std::remove_if(edges_.begin(), edges_.end(),
                              [&id](const GraphEdge& edge) {
                                return edge.from_node == id || edge.to_node == id;
                              }),
               edges_.end());
  nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(),
                              [&id](const std::unique_ptr<INodeModel>& node) {
                                return node->Id() == id;
                              }),
               nodes_.end());
}

auto PipelineGraph::FindNode(const NodeId& id) -> INodeModel* {
  return const_cast<INodeModel*>(static_cast<const PipelineGraph*>(this)->FindNode(id));
}

auto PipelineGraph::ApplyBackboneEdit(const std::vector<GraphEdge>& disconnected,
                                      std::vector<GraphEdge>        connected,
                                      std::unique_ptr<INodeModel> inserted, const NodeId& removed)
    -> std::vector<GraphValidationError> {
  if ((inserted && (!removed.Empty() || FindNode(inserted->Id()))) ||
      (!removed.Empty() && !FindNode(removed))) {
    throw std::invalid_argument("Invalid backbone node insertion/removal");
  }
  const auto matches = [](const GraphEdge& a, const GraphEdge& b) {
    return a.from_node == b.from_node && a.from_port == b.from_port && a.to_node == b.to_node &&
           a.to_port == b.to_port;
  };
  std::vector<std::pair<std::size_t, GraphEdge>> retained;
  for (std::size_t i = 0; i < edges_.size(); ++i) {
    const auto& edge = edges_[i];
    if ((!removed.Empty() && (edge.from_node == removed || edge.to_node == removed)) ||
        std::any_of(disconnected.begin(), disconnected.end(),
                    [&](const auto& other) { return matches(edge, other); })) {
      retained.emplace_back(i, edge);
    }
  }
  // Reserve both forward and restoration capacity before changing any live state.
  edges_.reserve(edges_.size() + connected.size());
  nodes_.reserve(nodes_.size() + (inserted ? 1 : 0));
  const auto                  removed_index = static_cast<std::size_t>(std::distance(
      nodes_.begin(), std::find_if(nodes_.begin(), nodes_.end(),
                                                    [&](const auto& node) { return node->Id() == removed; })));
  std::unique_ptr<INodeModel> retained_node;
  const bool                  has_insert = inserted != nullptr;
  for (auto it = retained.rbegin(); it != retained.rend(); ++it) {
    edges_.erase(edges_.begin() + it->first);
  }
  const auto kept_edge_count = edges_.size();
  for (auto& edge : connected) edges_.push_back(std::move(edge));
  if (has_insert) nodes_.push_back(std::move(inserted));
  if (!removed.Empty()) {
    retained_node = std::move(nodes_[removed_index]);
    nodes_.erase(nodes_.begin() + removed_index);
  }
  const auto restore = [&] {
    edges_.resize(kept_edge_count);
    for (auto& [index, edge] : retained) {
      edges_.insert(edges_.begin() + index, std::move(edge));
    }
    if (has_insert) nodes_.pop_back();
    if (retained_node) {
      nodes_.insert(nodes_.begin() + removed_index, std::move(retained_node));
    }
  };
  try {
    auto errors   = Validate();
    auto backbone = ValidateImageBackbone();
    errors.insert(errors.end(), backbone.begin(), backbone.end());
    if (!errors.empty()) restore();
    return errors;
  } catch (...) {
    restore();
    throw;
  }
}

auto PipelineGraph::FindNode(const NodeId& id) const -> const INodeModel* {
  for (const auto& node : nodes_) {
    if (node->Id() == id) {
      return node.get();
    }
  }
  return nullptr;
}

auto PipelineGraph::FindNode(std::string_view id) -> INodeModel* {
  return FindNode(NodeId{std::string{id}});
}

auto PipelineGraph::FindNode(std::string_view id) const -> const INodeModel* {
  return FindNode(NodeId{std::string{id}});
}

auto PipelineGraph::FindPort(const INodeModel& node, const PortId& id, bool input) const
    -> const PortDescriptor* {
  const auto ports = input ? node.InputPorts() : node.OutputPorts();
  for (const auto& port : ports) {
    if (port.id == id) {
      return &port;
    }
  }
  return nullptr;
}

auto PipelineGraph::Validate() const -> std::vector<GraphValidationError> {
  std::vector<GraphValidationError> errors;
  int develop_count = 0;
  int drt_count     = 0;
  for (const auto& node : nodes_) {
    if (node->Type() == type_ids::DevelopNode()) {
      ++develop_count;
    }
    if (node->Type() == type_ids::DrtNode()) {
      ++drt_count;
    }
  }
  if (develop_count == 0) {
    errors.push_back({GraphValidationCode::MissingDevelopEndpoint, "Graph needs one Develop node"});
  } else if (develop_count > 1) {
    errors.push_back(
        {GraphValidationCode::MultipleDevelopEndpoints, "Graph has more than one Develop node"});
  }
  if (drt_count == 0) {
    errors.push_back({GraphValidationCode::MissingDrtEndpoint, "Graph needs one DRT node"});
  } else if (drt_count > 1) {
    errors.push_back({GraphValidationCode::MultipleDrtEndpoints, "Graph has more than one DRT node"});
  }

  std::unordered_map<std::string, int> incoming_count;
  for (const auto& edge : edges_) {
    const auto* from = FindNode(edge.from_node);
    const auto* to   = FindNode(edge.to_node);
    if (from == nullptr) {
      errors.push_back({GraphValidationCode::UnknownNode,
                        "Unknown from node: " + std::string{edge.from_node.Value()}});
      continue;
    }
    if (to == nullptr) {
      errors.push_back({GraphValidationCode::UnknownNode,
                        "Unknown to node: " + std::string{edge.to_node.Value()}});
      continue;
    }
    const auto* from_port = FindPort(*from, edge.from_port, false);
    const auto* to_port   = FindPort(*to, edge.to_port, true);
    if (from_port == nullptr) {
      errors.push_back({GraphValidationCode::UnknownPort,
                        "Unknown output port: " + std::string{edge.from_port.Value()}});
      continue;
    }
    if (to_port == nullptr) {
      errors.push_back({GraphValidationCode::UnknownPort,
                        "Unknown input port: " + std::string{edge.to_port.Value()}});
      continue;
    }
    if (from_port->data_type != to_port->data_type) {
      errors.push_back({GraphValidationCode::PortTypeMismatch,
                        "Port type mismatch: " + std::string{from->Id().Value()} + "." +
                            std::string{from_port->id.Value()} + " -> " +
                            std::string{to->Id().Value()} + "." + std::string{to_port->id.Value()}});
    }
    const std::string key =
        std::string{to->Id().Value()} + "/" + std::string{to_port->id.Value()};
    incoming_count[key] += 1;
  }

  for (const auto& node : nodes_) {
    for (const auto& port : node->InputPorts()) {
      const std::string key = std::string{node->Id().Value()} + "/" + std::string{port.id.Value()};
      const int         count = incoming_count[key];
      if (port.required && count == 0) {
        errors.push_back({GraphValidationCode::MissingRequiredInput,
                          "Missing required input: " + key});
      }
      if (count > 1) {
        errors.push_back({GraphValidationCode::MultipleInputsOnPort, "Multiple inputs on " + key});
      }
    }
  }

  std::unordered_map<std::string, int> indegree;
  std::unordered_map<std::string, std::vector<std::string>> adjacency;
  for (const auto& node : nodes_) {
    indegree[std::string{node->Id().Value()}] = 0;
  }
  for (const auto& edge : edges_) {
    adjacency[std::string{edge.from_node.Value()}].push_back(std::string{edge.to_node.Value()});
    indegree[std::string{edge.to_node.Value()}] += 1;
  }
  std::queue<std::string> ready;
  for (const auto& [id, degree] : indegree) {
    if (degree == 0) {
      ready.push(id);
    }
  }
  std::size_t visited = 0;
  while (!ready.empty()) {
    const auto id = ready.front();
    ready.pop();
    ++visited;
    for (const auto& next : adjacency[id]) {
      indegree[next] -= 1;
      if (indegree[next] == 0) {
        ready.push(next);
      }
    }
  }
  if (visited != nodes_.size()) {
    errors.push_back({GraphValidationCode::Cycle, "PipelineGraph contains a cycle"});
  }
  return errors;
}

auto PipelineGraph::ImageBackboneNodeIds() const -> std::vector<NodeId> {
  const INodeModel* develop = nullptr;
  const INodeModel* drt      = nullptr;
  int               develop_count = 0;
  int               drt_count      = 0;
  for (const auto& node : nodes_) {
    if (node->Type() == type_ids::DevelopNode()) {
      ++develop_count;
      develop = node.get();
    }
    if (node->Type() == type_ids::DrtNode()) {
      ++drt_count;
      drt = node.get();
    }
  }
  if (develop == nullptr || drt == nullptr || develop_count != 1 || drt_count != 1) {
    return {};
  }

  std::vector<NodeId> path;
  std::unordered_set<std::string> visited;
  NodeId current = develop->Id();
  while (true) {
    const std::string key{current.Value()};
    if (visited.contains(key)) {
      return {};
    }
    visited.insert(key);
    path.push_back(current);
    const auto* node = FindNode(current);
    if (node == nullptr) {
      return {};
    }
    std::vector<NodeId> outgoing;
    for (const auto& edge : edges_) {
      if (edge.from_node != current) {
        continue;
      }
      const auto* from_port = FindPort(*node, edge.from_port, false);
      if (from_port == nullptr || from_port->data_type != PortDataType::SceneImage) {
        continue;
      }
      outgoing.push_back(edge.to_node);
    }
    if (outgoing.size() > 1) {
      return {};
    }
    if (outgoing.empty()) {
      if (current == drt->Id()) {
        return path;
      }
      return {};
    }
    current = outgoing.front();
  }
}

auto PipelineGraph::ValidateImageBackbone() const -> std::vector<GraphValidationError> {
  std::vector<GraphValidationError> errors;
  const INodeModel* develop = nullptr;
  const INodeModel* drt      = nullptr;
  for (const auto& node : nodes_) {
    if (node->Type() == type_ids::DevelopNode()) {
      develop = node.get();
    }
    if (node->Type() == type_ids::DrtNode()) {
      drt = node.get();
    }
  }
  if (develop == nullptr || drt == nullptr) {
    errors.push_back({GraphValidationCode::BrokenImageBackbone,
                       "Image backbone requires one Develop and one DRT"});
    return errors;
  }

  for (const auto& node : nodes_) {
    std::size_t scene_out = 0;
    for (const auto& edge : edges_) {
      if (edge.from_node != node->Id()) {
        continue;
      }
      const auto* from_port = FindPort(*node, edge.from_port, false);
      if (from_port != nullptr && from_port->data_type == PortDataType::SceneImage) {
        ++scene_out;
      }
    }
    if (scene_out > 1) {
      errors.push_back({GraphValidationCode::SceneImageFanOut,
                        "Scene-image fan-out from " + std::string{node->Id().Value()}});
    }
  }

  const auto path = ImageBackboneNodeIds();
  if (path.empty()) {
    errors.push_back({GraphValidationCode::BrokenImageBackbone,
                       "Graph has no unique Develop to DRT scene-image path"});
    return errors;
  }
  if (path.front() != develop->Id() || path.back() != drt->Id()) {
    errors.push_back({GraphValidationCode::BrokenImageBackbone,
                       "Image backbone does not start at Develop and end at DRT"});
  }

  std::unordered_set<std::string> on_path;
  for (const auto& id : path) {
    on_path.insert(std::string{id.Value()});
    const auto* node = FindNode(id);
    if (node == nullptr) {
      continue;
    }
    const auto& type = node->Type();
    if (type != type_ids::DevelopNode() && type != type_ids::ColorGradeNode() &&
        type != type_ids::DrtNode()) {
      errors.push_back({GraphValidationCode::BrokenImageBackbone,
                        "Non-image node on backbone: " + std::string{id.Value()}});
    }
  }
  for (const auto& node : nodes_) {
    if (node->Type() != type_ids::ColorGradeNode()) {
      continue;
    }
    if (!on_path.contains(std::string{node->Id().Value()})) {
      errors.push_back({GraphValidationCode::ColorGradeNotOnImageBackbone,
                        "Color Grade is not on the image backbone: " +
                            std::string{node->Id().Value()}});
    }
  }
  return errors;
}

auto PipelineGraph::TopologicalOrder() const -> std::vector<NodeId> {
  if (!Validate().empty()) {
    throw std::runtime_error("TopologicalOrder requires a valid graph");
  }
  std::unordered_map<std::string, int> indegree;
  std::unordered_map<std::string, std::vector<std::string>> adjacency;
  for (const auto& node : nodes_) {
    indegree[std::string{node->Id().Value()}] = 0;
  }
  for (const auto& edge : edges_) {
    adjacency[std::string{edge.from_node.Value()}].push_back(std::string{edge.to_node.Value()});
    indegree[std::string{edge.to_node.Value()}] += 1;
  }
  std::queue<std::string> ready;
  for (const auto& node : nodes_) {
    if (indegree[std::string{node->Id().Value()}] == 0) {
      ready.push(std::string{node->Id().Value()});
    }
  }
  std::vector<NodeId> order;
  while (!ready.empty()) {
    const auto id = ready.front();
    ready.pop();
    order.emplace_back(id);
    for (const auto& next : adjacency[id]) {
      indegree[next] -= 1;
      if (indegree[next] == 0) {
        ready.push(next);
      }
    }
  }
  return order;
}

}  // namespace alcedo
