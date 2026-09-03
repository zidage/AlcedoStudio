//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <cstdint>
#include <map>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "app/editor_node_graph_projection.hpp"
#include "edit/graph/graph_ids.hpp"
#include "qanGraph.h"

namespace qan {
class Edge;
class Node;
class PortItem;
}  // namespace qan

namespace alcedo::ui {

/**
 * @brief Result of applying an immutable node-graph snapshot to a Qan graph.
 *
 * @p succeeded is true only when the live Qan primitives match @p snapshot.
 * Topology rebuilds that fail restore the prior complete projection before
 * returning. The adapter never mutates PipelineDocument.
 */
struct AlcedoQanGraphApplyResult {
  bool    succeeded        = false;
  bool    rebuilt_topology = false;
  QString error;
};

/**
 * @brief Maps an EditorNodeGraphSnapshot onto documented QuickQanava primitives.
 *
 * Ownership: this object does not own the Qan graph. The QML Loader / GraphView
 * owns @c qan::Graph. Identity maps hold QPointer values and are cleared before
 * any mapped primitive is destroyed. A Qan node pointer is never a NodeId.
 *
 * Threading: GUI thread only. ApplySnapshot and identity queries must run on
 * the thread that owns the Qan graph.
 *
 * Side effects: insert, remove, port bind, and label updates on the bound Qan
 * graph. No history commit, no photo render, and no domain graph mutation.
 */
class AlcedoQanGraph : public QObject {
  Q_OBJECT
  Q_PROPERTY(qan::Graph* graph READ graph WRITE set_graph NOTIFY GraphChanged)

 public:
  explicit AlcedoQanGraph(QObject* parent = nullptr);
  ~AlcedoQanGraph() override;

  /**
   * @brief Bind the Qan graph that receives projected primitives.
   * @param graph Loader-owned graph, or nullptr to drop identity maps. The
   *        previous graph is not cleared; only adapter maps are dropped.
   */
  void               set_graph(qan::Graph* graph);
  [[nodiscard]] auto graph() const -> qan::Graph*;

  /**
   * @brief Project @p snapshot onto the bound Qan graph.
   *
   * Same session and topology revision with matching node/edge identities
   * updates labels and Mask roles in place. A generation or topology change
   * replaces every primitive. A stale generation, topology, or projection
   * revision is rejected without mutation.
   *
   * @pre GUI thread. @c graph() is a completed Qan graph with port delegates.
   * @return Success with @c rebuilt_topology set when primitives were replaced.
   *         On Qan insert/bind failure, the prior complete projection is
   *         restored when one existed.
   */
  [[nodiscard]] auto ApplySnapshot(const EditorNodeGraphSnapshot& snapshot)
      -> AlcedoQanGraphApplyResult;

  /**
   * @return Live Qan node for @p node_id in the current generation, or nullptr.
   */
  [[nodiscard]] auto NodeFor(const NodeId& node_id) const -> qan::Node*;

  /**
   * @return Live Qan edge for the projected backbone identity, or nullptr.
   */
  [[nodiscard]] auto EdgeFor(const EditorNodeEdgeProjection& edge) const -> qan::Edge*;

  /**
   * @return Top input port for @p node_id and @p port_id, or nullptr.
   */
  [[nodiscard]] auto InputPortFor(const NodeId& node_id, const PortId& port_id) const
      -> qan::PortItem*;

  /**
   * @return Bottom output port for @p node_id and @p port_id, or nullptr.
   */
  [[nodiscard]] auto OutputPortFor(const NodeId& node_id, const PortId& port_id) const
      -> qan::PortItem*;

  /**
   * @brief Resolve a Qan node to a product NodeId.
   *
   * Rejects nullptr, destroyed primitives, unknown pointers, and primitives
   * recorded under another session generation. This is the only selection
   * identity the adapter exposes; a raw Qan selected-node list is not a
   * product selection source.
   */
  [[nodiscard]] auto LiveNodeId(const qan::Node* node) const -> std::optional<NodeId>;

  /**
   * @return Copied node-card values for @p node_id, or nullptr when unmapped.
   */
  [[nodiscard]] auto NodeProjection(const NodeId& node_id) const -> const EditorNodeProjection*;

  [[nodiscard]] auto session_generation() const -> std::uint64_t;
  [[nodiscard]] auto projection_revision() const -> std::uint64_t;
  [[nodiscard]] auto topology_revision() const -> std::uint64_t;
  [[nodiscard]] auto has_projection() const -> bool;

 signals:
  void GraphChanged();

 protected:
  /**
   * @brief Documented @c qan::Graph::insertNode() entry used during rebuilds.
   *
   * Tests may override this to inject a nullptr result. Production always
   * forwards to the bound graph.
   */
  [[nodiscard]] virtual auto InsertQanNode(qan::Graph& graph) -> qan::Node*;

 private:
  struct EdgeKey {
    NodeId      source_node_id;
    PortId      source_port_id;
    NodeId      destination_node_id;
    PortId      destination_port_id;

    friend auto operator<(const EdgeKey& lhs, const EdgeKey& rhs) -> bool {
      if (lhs.source_node_id != rhs.source_node_id) {
        return lhs.source_node_id < rhs.source_node_id;
      }
      if (lhs.source_port_id != rhs.source_port_id) {
        return lhs.source_port_id < rhs.source_port_id;
      }
      if (lhs.destination_node_id != rhs.destination_node_id) {
        return lhs.destination_node_id < rhs.destination_node_id;
      }
      return lhs.destination_port_id < rhs.destination_port_id;
    }

    friend auto operator==(const EdgeKey& lhs, const EdgeKey& rhs) -> bool {
      return lhs.source_node_id == rhs.source_node_id && lhs.source_port_id == rhs.source_port_id &&
             lhs.destination_node_id == rhs.destination_node_id &&
             lhs.destination_port_id == rhs.destination_port_id;
    }
  };

  struct NodePorts {
    std::map<PortId, QPointer<qan::PortItem>> inputs;
    std::map<PortId, QPointer<qan::PortItem>> outputs;
  };

  struct ReverseNode {
    NodeId        node_id;
    std::uint64_t session_generation = 0;
  };

  void               ClearIdentityMaps();
  void               OnGraphDestroyed();
  void               DestroyMappedPrimitives();
  void               DestroyPrimitives(const std::vector<QPointer<qan::Edge>>& edges,
                                       const std::vector<QPointer<qan::Node>>& nodes);

  [[nodiscard]] auto RejectIfStale(const EditorNodeGraphSnapshot& snapshot) const -> QString;
  [[nodiscard]] auto ValidateSnapshot(const EditorNodeGraphSnapshot& snapshot) const -> QString;
  [[nodiscard]] auto CanUpdateRoles(const EditorNodeGraphSnapshot& snapshot) const -> bool;
  auto ApplyRoles(const EditorNodeGraphSnapshot& snapshot) -> AlcedoQanGraphApplyResult;
  auto ReplaceTopology(const EditorNodeGraphSnapshot& snapshot) -> AlcedoQanGraphApplyResult;
  [[nodiscard]] auto InsertTopology(const EditorNodeGraphSnapshot& snapshot) -> QString;
  [[nodiscard]] auto InsertNodePorts(qan::Node& qan_node, const EditorNodeProjection& node)
      -> QString;
  [[nodiscard]] auto InsertPort(qan::Node& qan_node, const NodeId& node_id, const PortId& port_id,
                                bool is_input) -> qan::PortItem*;
  [[nodiscard]] auto InsertEdge(const EditorNodeEdgeProjection& edge) -> QString;

  [[nodiscard]] static auto MakeEdgeKey(const EditorNodeEdgeProjection& edge) -> EdgeKey;
  [[nodiscard]] static auto ToQString(std::string_view text) -> QString;
  [[nodiscard]] static auto QanPortId(bool is_input, const PortId& port_id) -> QString;

  QPointer<qan::Graph>      graph_;
  bool                      has_projection_      = false;
  bool                      rebuild_in_progress_ = false;
  EditorNodeGraphSnapshot   applied_;
  std::map<NodeId, QPointer<qan::Node>>             node_by_id_;
  std::map<EdgeKey, QPointer<qan::Edge>>            edge_by_key_;
  std::map<NodeId, NodePorts>                       ports_by_node_;
  std::map<NodeId, EditorNodeProjection>            node_projections_;
  std::unordered_map<const qan::Node*, ReverseNode> node_from_qan_;
};

}  // namespace alcedo::ui
