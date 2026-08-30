//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
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
 * Does not own GPU resources. @ref Validate reports endpoint, port-type, fan-in, and
 * cycle errors. @ref ValidateImageBackbone additionally requires a unique Develop to
 * DRT scene-image path that visits every Color Grade. Product graph edits go through
 * pipeline graph commands; this type does not expose a mutable node container.
 */
class PipelineGraph {
 public:
  void AddNode(std::unique_ptr<INodeModel> node);
  void Connect(NodeId from_node, PortId from_port, NodeId to_node, PortId to_port);
  /**
   * @brief Remove one exact edge. No-op if that edge is absent.
   */
  void Disconnect(const NodeId& from_node, const PortId& from_port, const NodeId& to_node,
                   const PortId& to_port);
  /**
   * @brief Remove @p id and every incident edge.
   * @throws std::invalid_argument if @p id is not in the graph.
   */
  void RemoveNode(const NodeId& id);

  [[nodiscard]] auto Validate() const -> std::vector<GraphValidationError>;
  /**
   * @brief Scene-image backbone rules on top of @ref Validate.
   *
   * Rejects scene-image fan-out, a path that does not run Develop to DRT, Color
   * Grades off that path, and non-image node types on the path. Mask edges are
   * ignored. Does not replace @ref Validate.
   */
  [[nodiscard]] auto ValidateImageBackbone() const -> std::vector<GraphValidationError>;
  /**
   * @brief Develop through Color Grades to DRT along unique scene-image edges.
   * @return Empty when the unique backbone cannot be walked.
   */
  [[nodiscard]] auto ImageBackboneNodeIds() const -> std::vector<NodeId>;
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
