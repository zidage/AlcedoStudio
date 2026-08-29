//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/graph/pipeline_graph.hpp"

#include <queue>
#include <stdexcept>
#include <string>
#include <unordered_map>

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

auto PipelineGraph::FindNode(const NodeId& id) -> INodeModel* {
  return const_cast<INodeModel*>(static_cast<const PipelineGraph*>(this)->FindNode(id));
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
