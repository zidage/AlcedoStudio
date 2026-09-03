//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/alcedo_qan_graph.hpp"

#include <cstddef>
#include <string>

#include "qanEdge.h"
#include "qanEdgeItem.h"
#include "qanNode.h"
#include "qanNodeItem.h"
#include "qanPortItem.h"

namespace alcedo::ui {
namespace {

auto ContainsNodeId(const std::vector<EditorNodeProjection>& nodes, const NodeId& id) -> bool {
  for (const auto& node : nodes) {
    if (node.node_id == id) {
      return true;
    }
  }
  return false;
}

}  // namespace

AlcedoQanGraph::AlcedoQanGraph(QObject* parent) : QObject(parent) {}

AlcedoQanGraph::~AlcedoQanGraph() {
  rebuild_in_progress_ = true;
  ClearIdentityMaps();
}

void AlcedoQanGraph::set_graph(qan::Graph* graph) {
  if (graph_.data() == graph) {
    return;
  }
  if (!graph_.isNull()) {
    disconnect(graph_.data(), nullptr, this, nullptr);
  }
  rebuild_in_progress_ = true;
  ClearIdentityMaps();
  has_projection_      = false;
  applied_             = {};
  rebuild_in_progress_ = false;
  graph_               = graph;
  if (!graph_.isNull()) {
    connect(graph_.data(), &QObject::destroyed, this, &AlcedoQanGraph::OnGraphDestroyed);
  }
  emit GraphChanged();
}

auto AlcedoQanGraph::graph() const -> qan::Graph* { return graph_.data(); }

auto AlcedoQanGraph::ApplySnapshot(const EditorNodeGraphSnapshot& snapshot)
    -> AlcedoQanGraphApplyResult {
  AlcedoQanGraphApplyResult result;
  if (graph_.isNull()) {
    result.error = QStringLiteral("AlcedoQanGraph has no Qan graph");
    return result;
  }
  const auto stale = RejectIfStale(snapshot);
  if (!stale.isEmpty()) {
    result.error = stale;
    return result;
  }
  const auto invalid = ValidateSnapshot(snapshot);
  if (!invalid.isEmpty()) {
    result.error = invalid;
    return result;
  }
  if (CanUpdateRoles(snapshot)) {
    return ApplyRoles(snapshot);
  }
  return ReplaceTopology(snapshot);
}

auto AlcedoQanGraph::NodeFor(const NodeId& node_id) const -> qan::Node* {
  const auto it = node_by_id_.find(node_id);
  if (it == node_by_id_.end()) {
    return nullptr;
  }
  return it->second.data();
}

auto AlcedoQanGraph::EdgeFor(const EditorNodeEdgeProjection& edge) const -> qan::Edge* {
  const auto it = edge_by_key_.find(MakeEdgeKey(edge));
  if (it == edge_by_key_.end()) {
    return nullptr;
  }
  return it->second.data();
}

auto AlcedoQanGraph::InputPortFor(const NodeId& node_id, const PortId& port_id) const
    -> qan::PortItem* {
  const auto node = ports_by_node_.find(node_id);
  if (node == ports_by_node_.end()) {
    return nullptr;
  }
  const auto port = node->second.inputs.find(port_id);
  if (port == node->second.inputs.end()) {
    return nullptr;
  }
  return port->second.data();
}

auto AlcedoQanGraph::OutputPortFor(const NodeId& node_id, const PortId& port_id) const
    -> qan::PortItem* {
  const auto node = ports_by_node_.find(node_id);
  if (node == ports_by_node_.end()) {
    return nullptr;
  }
  const auto port = node->second.outputs.find(port_id);
  if (port == node->second.outputs.end()) {
    return nullptr;
  }
  return port->second.data();
}

auto AlcedoQanGraph::LiveNodeId(const qan::Node* node) const -> std::optional<NodeId> {
  if (rebuild_in_progress_ || node == nullptr) {
    return std::nullopt;
  }
  const auto it = node_from_qan_.find(node);
  if (it == node_from_qan_.end()) {
    return std::nullopt;
  }
  if (!has_projection_ || it->second.session_generation != applied_.session_generation) {
    return std::nullopt;
  }
  if (NodeFor(it->second.node_id) != node) {
    return std::nullopt;
  }
  return it->second.node_id;
}

auto AlcedoQanGraph::NodeProjection(const NodeId& node_id) const -> const EditorNodeProjection* {
  const auto it = node_projections_.find(node_id);
  if (it == node_projections_.end()) {
    return nullptr;
  }
  return &it->second;
}

auto AlcedoQanGraph::session_generation() const -> std::uint64_t {
  return has_projection_ ? applied_.session_generation : 0;
}

auto AlcedoQanGraph::projection_revision() const -> std::uint64_t {
  return has_projection_ ? applied_.projection_revision : 0;
}

auto AlcedoQanGraph::topology_revision() const -> std::uint64_t {
  return has_projection_ ? applied_.topology_revision : 0;
}

auto AlcedoQanGraph::has_projection() const -> bool { return has_projection_; }

auto AlcedoQanGraph::InsertQanNode(qan::Graph& graph) -> qan::Node* { return graph.insertNode(); }

void AlcedoQanGraph::ClearIdentityMaps() {
  node_by_id_.clear();
  edge_by_key_.clear();
  ports_by_node_.clear();
  node_projections_.clear();
  node_from_qan_.clear();
}

void AlcedoQanGraph::OnGraphDestroyed() {
  rebuild_in_progress_ = true;
  ClearIdentityMaps();
  has_projection_ = false;
  applied_        = {};
  graph_.clear();
  rebuild_in_progress_ = false;
  emit GraphChanged();
}

void AlcedoQanGraph::DestroyMappedPrimitives() {
  std::vector<QPointer<qan::Edge>> edges;
  edges.reserve(edge_by_key_.size());
  for (const auto& [key, edge] : edge_by_key_) {
    edges.push_back(edge);
  }
  std::vector<QPointer<qan::Node>> nodes;
  nodes.reserve(node_by_id_.size());
  for (const auto& [id, node] : node_by_id_) {
    nodes.push_back(node);
  }
  DestroyPrimitives(edges, nodes);
}

void AlcedoQanGraph::DestroyPrimitives(const std::vector<QPointer<qan::Edge>>& edges,
                                       const std::vector<QPointer<qan::Node>>& nodes) {
  if (graph_.isNull()) {
    return;
  }
  for (const auto& edge : edges) {
    if (!edge.isNull()) {
      graph_->removeEdge(edge.data(), true);
    }
  }
  for (const auto& node : nodes) {
    if (!node.isNull()) {
      graph_->removeNode(node.data(), true);
    }
  }
}

auto AlcedoQanGraph::RejectIfStale(const EditorNodeGraphSnapshot& snapshot) const -> QString {
  if (!has_projection_) {
    return {};
  }
  if (snapshot.session_generation < applied_.session_generation) {
    return QStringLiteral("snapshot session generation is stale");
  }
  if (snapshot.session_generation != applied_.session_generation) {
    return {};
  }
  if (snapshot.topology_revision < applied_.topology_revision) {
    return QStringLiteral("snapshot topology revision is stale");
  }
  if (snapshot.topology_revision == applied_.topology_revision &&
      snapshot.projection_revision < applied_.projection_revision) {
    return QStringLiteral("snapshot projection revision is stale");
  }
  return {};
}

auto AlcedoQanGraph::ValidateSnapshot(const EditorNodeGraphSnapshot& snapshot) const -> QString {
  if (snapshot.nodes.empty()) {
    return QStringLiteral("snapshot has no nodes");
  }
  for (std::size_t i = 0; i < snapshot.nodes.size(); ++i) {
    if (snapshot.nodes[i].node_id.Empty()) {
      return QStringLiteral("snapshot contains an empty NodeId");
    }
    for (std::size_t j = i + 1; j < snapshot.nodes.size(); ++j) {
      if (snapshot.nodes[i].node_id == snapshot.nodes[j].node_id) {
        return QStringLiteral("snapshot contains a duplicate NodeId");
      }
    }
  }
  for (const auto& edge : snapshot.edges) {
    if (!ContainsNodeId(snapshot.nodes, edge.source_node_id) ||
        !ContainsNodeId(snapshot.nodes, edge.destination_node_id)) {
      return QStringLiteral("snapshot edge references an unknown node");
    }
  }
  return {};
}

auto AlcedoQanGraph::CanUpdateRoles(const EditorNodeGraphSnapshot& snapshot) const -> bool {
  if (!has_projection_ || graph_.isNull()) {
    return false;
  }
  if (snapshot.session_generation != applied_.session_generation ||
      snapshot.topology_revision != applied_.topology_revision) {
    return false;
  }
  if (snapshot.nodes.size() != applied_.nodes.size() ||
      snapshot.edges.size() != applied_.edges.size()) {
    return false;
  }
  for (std::size_t i = 0; i < snapshot.nodes.size(); ++i) {
    if (snapshot.nodes[i].node_id != applied_.nodes[i].node_id ||
        snapshot.nodes[i].node_kind != applied_.nodes[i].node_kind) {
      return false;
    }
    if (NodeFor(snapshot.nodes[i].node_id) == nullptr) {
      return false;
    }
  }
  for (std::size_t i = 0; i < snapshot.edges.size(); ++i) {
    if (MakeEdgeKey(snapshot.edges[i]) != MakeEdgeKey(applied_.edges[i])) {
      return false;
    }
    if (EdgeFor(snapshot.edges[i]) == nullptr) {
      return false;
    }
  }
  return true;
}

auto AlcedoQanGraph::ApplyRoles(const EditorNodeGraphSnapshot& snapshot)
    -> AlcedoQanGraphApplyResult {
  AlcedoQanGraphApplyResult result;
  for (const auto& node : snapshot.nodes) {
    auto* qan_node = NodeFor(node.node_id);
    if (qan_node == nullptr) {
      result.error = QStringLiteral("Qan node is missing a visual item");
      return result;
    }
    qan_node->setLabel(ToQString(node.display_name));
    node_projections_[node.node_id] = node;
  }
  applied_         = snapshot;
  has_projection_  = true;
  result.succeeded = true;
  return result;
}

auto AlcedoQanGraph::ReplaceTopology(const EditorNodeGraphSnapshot& snapshot)
    -> AlcedoQanGraphApplyResult {
  AlcedoQanGraphApplyResult result;
  result.rebuilt_topology = true;

  const auto previous     = applied_;
  const auto had_previous = has_projection_;
  rebuild_in_progress_    = true;
  DestroyMappedPrimitives();
  ClearIdentityMaps();

  auto error = InsertTopology(snapshot);
  if (error.isEmpty()) {
    applied_             = snapshot;
    has_projection_      = true;
    rebuild_in_progress_ = false;
    result.succeeded     = true;
    return result;
  }

  DestroyMappedPrimitives();
  ClearIdentityMaps();
  has_projection_ = false;
  applied_        = {};
  if (had_previous) {
    const auto restore_error = InsertTopology(previous);
    if (restore_error.isEmpty()) {
      applied_        = previous;
      has_projection_ = true;
    } else {
      error += QStringLiteral("; restoring the prior Qan projection failed: ") + restore_error;
    }
  }
  rebuild_in_progress_ = false;
  result.error         = error;
  return result;
}

auto AlcedoQanGraph::InsertTopology(const EditorNodeGraphSnapshot& snapshot) -> QString {
  if (graph_.isNull()) {
    return QStringLiteral("AlcedoQanGraph has no Qan graph");
  }
  for (const auto& node : snapshot.nodes) {
    auto* qan_node = InsertQanNode(*graph_);
    if (qan_node == nullptr) {
      return QStringLiteral("Qan node creation failed");
    }
    if (qan_node->getItem() == nullptr) {
      graph_->removeNode(qan_node, true);
      return QStringLiteral("Qan node is missing a visual item");
    }
    qan_node->setLabel(ToQString(node.display_name));
    node_by_id_[node.node_id]       = qan_node;
    node_projections_[node.node_id] = node;
    node_from_qan_[qan_node]        = ReverseNode{node.node_id, snapshot.session_generation};
    const auto port_error           = InsertNodePorts(*qan_node, node);
    if (!port_error.isEmpty()) {
      return port_error;
    }
  }
  for (const auto& edge : snapshot.edges) {
    const auto edge_error = InsertEdge(edge);
    if (!edge_error.isEmpty()) {
      return edge_error;
    }
  }
  return {};
}

auto AlcedoQanGraph::InsertNodePorts(qan::Node& qan_node, const EditorNodeProjection& node)
    -> QString {
  const PortId image{"image"};
  if (node.node_kind != EditorNodeKind::Develop) {
    if (InsertPort(qan_node, node.node_id, image, true) == nullptr) {
      return QStringLiteral("Qan port insertion failed");
    }
  }
  if (node.node_kind != EditorNodeKind::Drt) {
    if (InsertPort(qan_node, node.node_id, image, false) == nullptr) {
      return QStringLiteral("Qan port insertion failed");
    }
  }
  return {};
}

auto AlcedoQanGraph::InsertPort(qan::Node& qan_node, const NodeId& node_id, const PortId& port_id,
                                bool is_input) -> qan::PortItem* {
  if (graph_.isNull()) {
    return nullptr;
  }
  const auto dock = is_input ? qan::NodeItem::Dock::Top : qan::NodeItem::Dock::Bottom;
  const auto type = is_input ? qan::PortItem::Type::In : qan::PortItem::Type::Out;
  auto* port = graph_->insertPort(&qan_node, dock, type, QString(), QanPortId(is_input, port_id));
  if (port == nullptr) {
    return nullptr;
  }
  port->setMultiplicity(qan::PortItem::Multiplicity::Single);
  if (is_input) {
    ports_by_node_[node_id].inputs[port_id] = port;
  } else {
    ports_by_node_[node_id].outputs[port_id] = port;
  }
  return port;
}

auto AlcedoQanGraph::InsertEdge(const EditorNodeEdgeProjection& edge) -> QString {
  auto* source = NodeFor(edge.source_node_id);
  auto* dest   = NodeFor(edge.destination_node_id);
  auto* out    = OutputPortFor(edge.source_node_id, edge.source_port_id);
  auto* in     = InputPortFor(edge.destination_node_id, edge.destination_port_id);
  if (source == nullptr || dest == nullptr) {
    return QStringLiteral("snapshot edge references an unknown node");
  }
  if (out == nullptr || in == nullptr) {
    return QStringLiteral("Qan edge port binding failed");
  }
  auto* qan_edge = graph_->insertEdge(source, dest);
  if (qan_edge == nullptr || qan_edge->getItem() == nullptr) {
    return QStringLiteral("Qan edge creation failed");
  }
  graph_->bindEdge(qan_edge, out, in);
  if (qan_edge->getItem()->getSourceItem() != out ||
      qan_edge->getItem()->getDestinationItem() != in) {
    graph_->removeEdge(qan_edge, true);
    return QStringLiteral("Qan edge port binding failed");
  }
  edge_by_key_[MakeEdgeKey(edge)] = qan_edge;
  return {};
}

auto AlcedoQanGraph::MakeEdgeKey(const EditorNodeEdgeProjection& edge) -> EdgeKey {
  return EdgeKey{edge.source_node_id, edge.source_port_id, edge.destination_node_id,
                 edge.destination_port_id};
}

auto AlcedoQanGraph::ToQString(std::string_view text) -> QString {
  return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

auto AlcedoQanGraph::QanPortId(bool is_input, const PortId& port_id) -> QString {
  return (is_input ? QStringLiteral("in:") : QStringLiteral("out:")) + ToQString(port_id.Value());
}

}  // namespace alcedo::ui
