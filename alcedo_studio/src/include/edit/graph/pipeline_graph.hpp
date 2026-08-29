//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "edit/graph/graph_validation.hpp"
#include "edit/graph/i_node_model.hpp"

namespace alcedo {

struct GraphEdge {
  NodeId from_node;
  PortId from_port;
  NodeId to_node;
  PortId to_port;
};

/**
 * @brief Simple directed graph of INodeModel instances.
 *
 * Does not own GPU resources. Validate reports endpoint, port-type, and cycle errors.
 */
class PipelineGraph {
 public:
  void AddNode(std::unique_ptr<INodeModel> node);
  void Connect(NodeId from_node, PortId from_port, NodeId to_node, PortId to_port);

  [[nodiscard]] auto Validate() const -> std::vector<GraphValidationError>;
  /**
   * @brief Topological node order.
   * @pre Validate() is empty; otherwise throws std::runtime_error.
   */
  [[nodiscard]] auto TopologicalOrder() const -> std::vector<NodeId>;

  [[nodiscard]] auto NodeCount() const -> std::size_t { return nodes_.size(); }
  [[nodiscard]] auto Edges() const -> const std::vector<GraphEdge>& { return edges_; }

  [[nodiscard]] auto FindNode(const NodeId& id) -> INodeModel*;
  [[nodiscard]] auto FindNode(const NodeId& id) const -> const INodeModel*;
  [[nodiscard]] auto FindNode(std::string_view id) -> INodeModel*;
  [[nodiscard]] auto FindNode(std::string_view id) const -> const INodeModel*;

  [[nodiscard]] auto Nodes() -> std::vector<std::unique_ptr<INodeModel>>& { return nodes_; }
  [[nodiscard]] auto Nodes() const -> const std::vector<std::unique_ptr<INodeModel>>& {
    return nodes_;
  }

 private:
  [[nodiscard]] auto FindPort(const INodeModel& node, const PortId& id, bool input) const
      -> const PortDescriptor*;

  std::vector<std::unique_ptr<INodeModel>> nodes_;
  std::vector<GraphEdge>                   edges_;
};

}  // namespace alcedo
