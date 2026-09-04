//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <functional>
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

  friend auto operator==(const GraphEdge& lhs, const GraphEdge& rhs) -> bool {
    return lhs.from_node == rhs.from_node && lhs.from_port == rhs.from_port &&
           lhs.to_node == rhs.to_node && lhs.to_port == rhs.to_port;
  }
};

/// One node taken from a live graph by vector index.
struct TopologyNodeRemoval {
  NodeId      id;
  std::size_t original_index = 0;
};

/// One node inserted into a live graph at a final vector index.
struct TopologyNodeInsertion {
  std::unique_ptr<INodeModel> node;
  std::size_t                 final_index = 0;
};

/// One edge taken from a live graph by vector index.
struct TopologyEdgeRemoval {
  GraphEdge   edge;
  std::size_t original_index = 0;
};

/// One edge inserted into a live graph at a final vector index.
struct TopologyEdgeInsertion {
  GraphEdge   edge;
  std::size_t final_index = 0;
};

/// Test hook invoked after each in-place topology mutation step.
/// @p step is disconnect, remove_node, insert_node, connect, or validate.
using TopologyDeltaStepHook = std::function<void(std::string_view step, std::size_t index)>;

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

  /**
   * @brief Apply one backbone edit, retaining only removed edges and the removed node.
   * @pre Caller holds the live executor render lock. Insert/remove are mutually exclusive.
   * @return Validation errors after restoring exact edge order and node ownership, or empty.
   * All storage is reserved before mutation; exceptions restore the same objects and propagate.
   */
  auto ApplyBackboneEdit(const std::vector<GraphEdge>& disconnected,
                         std::vector<GraphEdge>        connected,
                         std::unique_ptr<INodeModel> inserted = nullptr, const NodeId& removed = {})
      -> std::vector<GraphValidationError>;

  /**
   * @brief Apply one net topology delta in place on this live graph object.
   *
   * Removed nodes and disconnected edges must match the live vectors at the
   * stored original indexes. Inserted NodeIds and connected edge identities
   * must not already exist after those removals. All restoration storage is
   * reserved before mutation. Unaffected INodeModel addresses stay stable.
   *
   * @pre Caller holds the live executor render lock.
   * @return Validation errors after restoring exact node ownership, node order,
   *         and edge order, or empty on success. Exceptions restore the same
   *         objects and propagate.
   */
  auto ApplyTopologyDelta(const std::vector<TopologyNodeRemoval>&     removed_nodes,
                          std::vector<TopologyNodeInsertion>          inserted_nodes,
                          const std::vector<TopologyEdgeRemoval>&     disconnected_edges,
                          const std::vector<TopologyEdgeInsertion>&   connected_edges,
                          TopologyDeltaStepHook                       after_step = {},
                          std::vector<std::unique_ptr<INodeModel>>*   discarded_nodes = nullptr)
      -> std::vector<GraphValidationError>;

  [[nodiscard]] auto Validate() const -> std::vector<GraphValidationError>;
  /**
   * @brief Scene-image backbone rules on top of @ref Validate.
   *
   * Rejects scene-image fan-out, a path that does not run Develop to DRT, Color
   * Grades off that path, and non-image node types on the path. Color Grade has
   * no Mask graph port. Does not replace @ref Validate.
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
