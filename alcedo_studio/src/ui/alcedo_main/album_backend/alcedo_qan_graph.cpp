//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/alcedo_qan_graph.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QEvent>
#include <QMetaMethod>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QVariant>
#include <QVariantMap>
#include <algorithm>
#include <cstddef>
#include <iterator>
#include <optional>
#include <string>

#include "qanConnector.h"
#include "qanEdge.h"
#include "qanEdgeItem.h"
#include "qanNode.h"
#include "qanNodeItem.h"
#include "qanPortItem.h"
#include "qanStyle.h"
#include "ui/alcedo_main/app_theme.hpp"

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

/// QuickQanava parents NodeItem/EdgeItem to the view container and destroys
/// them with deleteLater. Hide and unparent immediately so a topology rebuild
/// cannot leave the previous cards on screen while the photo has already
/// moved on.
void HideDetachedVisual(QQuickItem* item) {
  if (item == nullptr) {
    return;
  }
  item->setVisible(false);
  item->setParentItem(nullptr);
}

void FlushDeferredDeletes() { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }

}  // namespace

AlcedoQanGraph::AlcedoQanGraph(QObject* parent) : QObject(parent) {
  connect(&AppTheme::Instance(), &AppTheme::ThemeChanged, this, [this]() {
    ConfigureConnector();
    for (const auto& [key, candidate] : edge_candidate_) {
      const auto it = edge_by_key_.find(key);
      if (it == edge_by_key_.end() || it->second.isNull()) {
        continue;
      }
      ApplyEdgePresentation(*it->second, candidate);
    }
  });
}

AlcedoQanGraph::~AlcedoQanGraph() {
  rebuild_in_progress_ = true;
  ClearDrawerConnections();
  ClearIdentityMaps();
  delegate_library_.Reset();
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
  has_projection_ = false;
  applied_        = {};
  delegate_library_.Reset();
  rebuild_in_progress_ = false;
  graph_               = graph;
  if (!graph_.isNull()) {
    connect(graph_.data(), &QObject::destroyed, this, &AlcedoQanGraph::OnGraphDestroyed);
    ConfigureGraphPolicy();
  }
  emit GraphChanged();
}

void AlcedoQanGraph::set_color_grade_delegate_url(const QUrl& url) {
  if (delegate_library_.ColorGradeDelegateUrl() == url) {
    return;
  }
  delegate_library_.Configure(
      url, delegate_library_.EndpointDelegateUrl(), delegate_library_.PortDelegateUrl(),
      delegate_library_.PortDockDelegateUrl(), delegate_library_.EdgeDelegateUrl());
  emit DelegatesChanged();
}

auto AlcedoQanGraph::color_grade_delegate_url() const -> QUrl {
  return delegate_library_.ColorGradeDelegateUrl();
}

void AlcedoQanGraph::set_endpoint_delegate_url(const QUrl& url) {
  if (delegate_library_.EndpointDelegateUrl() == url) {
    return;
  }
  delegate_library_.Configure(
      delegate_library_.ColorGradeDelegateUrl(), url, delegate_library_.PortDelegateUrl(),
      delegate_library_.PortDockDelegateUrl(), delegate_library_.EdgeDelegateUrl());
  emit DelegatesChanged();
}

auto AlcedoQanGraph::endpoint_delegate_url() const -> QUrl {
  return delegate_library_.EndpointDelegateUrl();
}

void AlcedoQanGraph::set_port_delegate_url(const QUrl& url) {
  if (delegate_library_.PortDelegateUrl() == url) {
    return;
  }
  delegate_library_.Configure(
      delegate_library_.ColorGradeDelegateUrl(), delegate_library_.EndpointDelegateUrl(), url,
      delegate_library_.PortDockDelegateUrl(), delegate_library_.EdgeDelegateUrl());
  emit DelegatesChanged();
}

auto AlcedoQanGraph::port_delegate_url() const -> QUrl {
  return delegate_library_.PortDelegateUrl();
}

void AlcedoQanGraph::set_port_dock_delegate_url(const QUrl& url) {
  if (delegate_library_.PortDockDelegateUrl() == url) {
    return;
  }
  delegate_library_.Configure(
      delegate_library_.ColorGradeDelegateUrl(), delegate_library_.EndpointDelegateUrl(),
      delegate_library_.PortDelegateUrl(), url, delegate_library_.EdgeDelegateUrl());
  emit DelegatesChanged();
}

auto AlcedoQanGraph::port_dock_delegate_url() const -> QUrl {
  return delegate_library_.PortDockDelegateUrl();
}

void AlcedoQanGraph::set_edge_delegate_url(const QUrl& url) {
  if (delegate_library_.EdgeDelegateUrl() == url) {
    return;
  }
  delegate_library_.Configure(
      delegate_library_.ColorGradeDelegateUrl(), delegate_library_.EndpointDelegateUrl(),
      delegate_library_.PortDelegateUrl(), delegate_library_.PortDockDelegateUrl(), url);
  emit DelegatesChanged();
}

auto AlcedoQanGraph::edge_delegate_url() const -> QUrl {
  return delegate_library_.EdgeDelegateUrl();
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
  const auto it = std::find_if(applied_.nodes.begin(), applied_.nodes.end(),
                               [&](const auto& node) { return node.node_id == node_id; });
  if (it == applied_.nodes.end()) {
    return nullptr;
  }
  return &*it;
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

auto AlcedoQanGraph::InsertQanNode(qan::Graph& graph, const EditorNodeProjection& node)
    -> qan::Node* {
  auto* component = ComponentFor(node.node_kind);
  if (component == nullptr) {
    return nullptr;
  }
  return graph.insertNode(component);
}

auto AlcedoQanGraph::InsertQanPort(qan::Graph& graph, qan::Node& node, bool is_input,
                                   const QString& port_id) -> qan::PortItem* {
  const auto dock = is_input ? qan::NodeItem::Dock::Top : qan::NodeItem::Dock::Bottom;
  const auto type = is_input ? qan::PortItem::Type::In : qan::PortItem::Type::Out;
  return graph.insertPort(&node, dock, type, QString(), port_id);
}

auto AlcedoQanGraph::InsertQanEdge(qan::Graph& graph, qan::Node& source, qan::Node& destination,
                                   QQmlComponent* component) -> qan::Edge* {
  return graph.insertEdge(&source, &destination, component);
}

auto AlcedoQanGraph::BindQanEdge(qan::Graph& graph, qan::Edge& edge, qan::PortItem& source,
                                 qan::PortItem& destination) -> bool {
  graph.bindEdge(&edge, &source, &destination);
  return true;
}

auto AlcedoQanGraph::RemoveQanEdge(qan::Graph& graph, qan::Edge& edge) -> bool {
  return graph.removeEdge(&edge, true);
}

auto AlcedoQanGraph::RemoveQanPort(qan::Graph& graph, qan::Node& node, qan::PortItem& port)
    -> bool {
  graph.removePort(&node, &port);
  return true;
}

auto AlcedoQanGraph::RemoveQanNode(qan::Graph& graph, qan::Node& node) -> bool {
  HideDetachedVisual(node.getItem());
  return graph.removeNode(&node, true);
}

void AlcedoQanGraph::ClearIdentityMaps() {
  ClearDrawerConnections();
  node_by_id_.clear();
  edge_by_key_.clear();
  edge_candidate_.clear();
  ports_by_node_.clear();
  node_from_qan_.clear();
}

void AlcedoQanGraph::OnGraphDestroyed() {
  rebuild_in_progress_ = true;
  ClearIdentityMaps();
  has_projection_ = false;
  applied_        = {};
  delegate_library_.Reset();
  graph_.clear();
  rebuild_in_progress_ = false;
  emit GraphChanged();
}

void AlcedoQanGraph::DestroyMappedPrimitives() {
  std::vector<EditorNodeEdgeProjection> edges;
  edges.reserve(edge_by_key_.size());
  for (const auto& [key, edge] : edge_by_key_) {
    Q_UNUSED(edge);
    edges.push_back(EditorNodeEdgeProjection{key.source_node_id, key.source_port_id,
                                             key.destination_node_id, key.destination_port_id});
  }
  for (const auto& edge : edges) {
    (void)RemoveEdgeVisual(edge);
  }

  std::vector<NodeId> nodes;
  nodes.reserve(node_by_id_.size());
  for (const auto& [id, node] : node_by_id_) {
    Q_UNUSED(node);
    nodes.push_back(id);
  }
  for (const auto& node_id : nodes) {
    (void)RemoveNodeVisual(node_id);
  }
}

void AlcedoQanGraph::AttachLiveVisuals() {
  if (graph_.isNull()) {
    return;
  }
  auto* container = graph_->getContainerItem();
  auto  attach    = [container](QQuickItem* item) {
    if (item == nullptr) {
      return;
    }
    item->setVisible(true);
    if (container != nullptr && item->parentItem() != container) {
      item->setParentItem(container);
    }
  };
  for (const auto& [id, node] : node_by_id_) {
    Q_UNUSED(id);
    if (!node.isNull()) {
      attach(node->getItem());
    }
  }
  for (const auto& [key, edge] : edge_by_key_) {
    Q_UNUSED(key);
    if (!edge.isNull()) {
      attach(edge->getItem());
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
    ApplyNodePresentation(*qan_node, node);
  }
  applied_        = snapshot;
  has_projection_ = true;
  BindDrawerSignals();
  result.succeeded = true;
  return result;
}

auto AlcedoQanGraph::ReplaceTopology(const EditorNodeGraphSnapshot& snapshot)
    -> AlcedoQanGraphApplyResult {
  AlcedoQanGraphApplyResult result;
  result.rebuilt_topology = true;
  ++topology_replace_count_;

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
    BindDrawerSignals();
    ApplyConnectablePolicy();
    result.succeeded = true;
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
      BindDrawerSignals();
    } else {
      error += QStringLiteral("; restoring the prior Qan projection failed: ") + restore_error;
    }
  }
  rebuild_in_progress_ = false;
  ApplyConnectablePolicy();
  result.error = error;
  return result;
}

auto AlcedoQanGraph::InsertTopology(const EditorNodeGraphSnapshot& snapshot) -> QString {
  if (graph_.isNull()) {
    return QStringLiteral("AlcedoQanGraph has no Qan graph");
  }
  auto* engine = qmlEngine(graph_.data());
  if (engine == nullptr) {
    return QStringLiteral("Qan graph has no QML engine");
  }
  const auto delegate_error = delegate_library_.EnsureLoaded(*engine, *graph_);
  if (!delegate_error.isEmpty()) {
    return delegate_error;
  }
  for (const auto& node : snapshot.nodes) {
    const auto error = InsertNodeVisual(node, snapshot.session_generation);
    if (!error.isEmpty()) {
      return error;
    }
  }
  for (const auto& edge : snapshot.edges) {
    const auto edge_error = InsertEdge(edge);
    if (!edge_error.isEmpty()) {
      return edge_error;
    }
  }
  AttachLiveVisuals();
  return {};
}

auto AlcedoQanGraph::InsertNodeVisual(const EditorNodeProjection& node,
                                      std::uint64_t               session_generation,
                                      const NodeVisualState*      restore_state) -> QString {
  if (graph_.isNull()) {
    return QStringLiteral("AlcedoQanGraph has no Qan graph");
  }
  if (NodeFor(node.node_id) != nullptr) {
    return QStringLiteral("Qan node already exists");
  }

  auto* qan_node = InsertQanNode(*graph_, node);
  if (qan_node == nullptr) {
    if (ComponentFor(node.node_kind) == nullptr) {
      return node.node_kind == EditorNodeKind::ColorGrade
                 ? QStringLiteral("Alcedo color-grade node delegate is not set")
                 : QStringLiteral("Alcedo endpoint node delegate is not set");
    }
    return QStringLiteral("Qan node creation failed");
  }
  if (qan_node->getItem() == nullptr) {
    QString error = QStringLiteral("Qan node is missing a visual item");
    if (!RemoveQanNode(*graph_, *qan_node)) {
      error += QStringLiteral("; reversing the incomplete Qan node failed");
    }
    return error;
  }

  ApplyNodePresentation(*qan_node, node);
  node_by_id_[node.node_id]    = qan_node;
  node_from_qan_[qan_node]     = ReverseNode{node.node_id, session_generation};
  ports_by_node_[node.node_id] = {};
  const auto port_error        = InsertNodePorts(*qan_node, node);
  if (!port_error.isEmpty()) {
    const auto cleanup_error = RemoveNodeVisual(node.node_id);
    if (!cleanup_error.isEmpty()) {
      return port_error + QStringLiteral("; reversing the incomplete Qan node failed: ") +
             cleanup_error;
    }
    return port_error;
  }

  if (restore_state != nullptr && qan_node->getItem() != nullptr) {
    if (restore_state->has_position) {
      qan_node->getItem()->setPosition(restore_state->position);
    }
    if (qan_node->getItem()->property("drawerOpen").isValid()) {
      qan_node->getItem()->setProperty("drawerOpen", restore_state->drawer_open);
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
  auto* port = InsertQanPort(*graph_, qan_node, is_input, QanPortId(is_input, port_id));
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
  return InsertEdgeVisual(edge, false);
}

auto AlcedoQanGraph::InsertEdgeVisual(const EditorNodeEdgeProjection& edge, bool candidate)
    -> QString {
  if (graph_.isNull()) {
    return QStringLiteral("AlcedoQanGraph has no Qan graph");
  }
  if (EdgeFor(edge) != nullptr) {
    return QStringLiteral("Qan edge already exists");
  }
  if (delegate_library_.EdgeComponent() == nullptr) {
    return QStringLiteral("Alcedo edge delegate is not loaded");
  }
  auto* source = NodeFor(edge.source_node_id);
  auto* dest   = NodeFor(edge.destination_node_id);
  auto* out    = OutputPortFor(edge.source_node_id, edge.source_port_id);
  auto* in     = InputPortFor(edge.destination_node_id, edge.destination_port_id);
  if (source == nullptr || dest == nullptr) {
    return QStringLiteral("snapshot edge references an unknown node");
  }
  if (out == nullptr) {
    return QStringLiteral("Qan source port is missing");
  }
  if (in == nullptr) {
    return QStringLiteral("Qan destination port is missing");
  }
  auto* qan_edge = InsertQanEdge(*graph_, *source, *dest, delegate_library_.EdgeComponent());
  if (qan_edge == nullptr) {
    return QStringLiteral("Qan edge creation failed");
  }
  auto cleanup_created_edge = [&](const QString& error) {
    const auto cleanup_error = RemoveCreatedEdge(*qan_edge);
    if (cleanup_error.isEmpty()) {
      return error;
    }
    return error + QStringLiteral("; reversing the incomplete Qan edge failed: ") + cleanup_error;
  };
  if (qan_edge->getItem() == nullptr) {
    return cleanup_created_edge(QStringLiteral("Qan edge is missing a visual item"));
  }
  if (auto* item = qan_edge->getItem()) {
    item->setSrcShape(qan::EdgeStyle::ArrowShape::None);
    item->setDstShape(qan::EdgeStyle::ArrowShape::None);
  }
  if (!BindQanEdge(*graph_, *qan_edge, *out, *in)) {
    return cleanup_created_edge(QStringLiteral("Qan edge binding failed"));
  }
  if (qan_edge->getItem()->getSourceItem() != out ||
      qan_edge->getItem()->getDestinationItem() != in) {
    return cleanup_created_edge(QStringLiteral("Qan edge did not bind to the requested ports"));
  }
  const auto key    = MakeEdgeKey(edge);
  edge_by_key_[key] = qan_edge;
  ApplyEdgePresentation(*qan_edge, candidate);
  edge_candidate_[key] = candidate;
  return {};
}

auto AlcedoQanGraph::InsertProjectedNode(const EditorNodeProjection& node)
    -> AlcedoQanGraphApplyResult {
  AlcedoQanGraphApplyResult result;
  if (graph_.isNull() || !has_projection_) {
    result.error = QStringLiteral("AlcedoQanGraph has no Qan graph");
    return result;
  }
  if (NodeFor(node.node_id) != nullptr) {
    result.error = QStringLiteral("Qan node already exists");
    return result;
  }
  auto* engine = qmlEngine(graph_.data());
  if (engine == nullptr) {
    result.error = QStringLiteral("Qan graph has no QML engine");
    return result;
  }
  const auto delegate_error = delegate_library_.EnsureLoaded(*engine, *graph_);
  if (!delegate_error.isEmpty()) {
    result.error = delegate_error;
    return result;
  }
  const auto error = InsertNodeVisual(node, applied_.session_generation);
  if (!error.isEmpty()) {
    result.error = error;
    return result;
  }
  auto* qan_node = NodeFor(node.node_id);
  BindDrawerSignal(qan_node->getItem(), node.node_id);
  applied_.nodes.push_back(node);
  AttachLiveVisuals();
  ApplyConnectablePolicy();
  result.succeeded = true;
  return result;
}

auto AlcedoQanGraph::RemoveProjectedNode(const NodeId& node_id) -> AlcedoQanGraphApplyResult {
  AlcedoQanGraphApplyResult result;
  if (graph_.isNull()) {
    result.error = QStringLiteral("AlcedoQanGraph has no Qan graph");
    return result;
  }
  if (NodeFor(node_id) == nullptr) {
    result.succeeded = true;
    return result;
  }
  struct RemovedEdge {
    EditorNodeEdgeProjection edge;
    bool                     candidate = false;
  };
  auto append_reversal_error = [](QString* all_errors, const QString& error) {
    if (error.isEmpty()) {
      return;
    }
    if (!all_errors->isEmpty()) {
      *all_errors += QStringLiteral("; ");
    }
    *all_errors += error;
  };
  std::vector<RemovedEdge> incident;
  for (const auto& [key, edge] : edge_by_key_) {
    if (key.source_node_id == node_id || key.destination_node_id == node_id) {
      incident.push_back(
          RemovedEdge{{key.source_node_id, key.source_port_id, key.destination_node_id,
                       key.destination_port_id},
                      edge_candidate_.contains(key) ? edge_candidate_.at(key) : false});
    }
  }
  std::vector<RemovedEdge> removed_edges;
  for (const auto& item : incident) {
    const auto error = RemoveEdgeVisual(item.edge);
    if (!error.isEmpty()) {
      QString reversal_error;
      for (auto it = removed_edges.rbegin(); it != removed_edges.rend(); ++it) {
        append_reversal_error(&reversal_error, InsertEdgeVisual(it->edge, it->candidate));
      }
      result.error = error;
      if (!reversal_error.isEmpty()) {
        result.error += QStringLiteral("; visual reversal failed: ") + reversal_error;
      }
      result.succeeded = false;
      return result;
    }
    removed_edges.push_back(item);
  }
  const bool selection_removed = product_selected_node_id_ == node_id;
  const auto node_error        = RemoveNodeVisual(node_id);
  if (!node_error.isEmpty()) {
    QString reversal_error;
    for (auto it = removed_edges.rbegin(); it != removed_edges.rend(); ++it) {
      const auto error = InsertEdgeVisual(it->edge, it->candidate);
      if (!error.isEmpty()) {
        reversal_error += (reversal_error.isEmpty() ? QString() : QStringLiteral("; ")) + error;
      }
    }
    result.error = node_error;
    if (!reversal_error.isEmpty()) {
      result.error += QStringLiteral("; visual reversal failed: ") + reversal_error;
    }
    result.succeeded = false;
    return result;
  }
  for (const auto& item : removed_edges) {
    EraseAppliedEdge(item.edge);
  }
  EraseAppliedNode(node_id);
  ClearDrawerConnections();
  BindDrawerSignals();
  if (selection_removed) {
    ApplyProductSelection(std::nullopt);
  } else {
    ApplyConnectablePolicy();
  }
  result.succeeded = true;
  return result;
}

auto AlcedoQanGraph::InsertProjectedEdge(const EditorNodeEdgeProjection& edge, bool candidate)
    -> AlcedoQanGraphApplyResult {
  AlcedoQanGraphApplyResult result;
  if (graph_.isNull()) {
    result.error = QStringLiteral("AlcedoQanGraph has no Qan graph");
    return result;
  }
  if (EdgeFor(edge) != nullptr) {
    ApplyEdgePresentation(*EdgeFor(edge), candidate);
    edge_candidate_[MakeEdgeKey(edge)] = candidate;
    result.succeeded                   = true;
    return result;
  }
  const auto error = InsertEdgeVisual(edge, candidate);
  if (!error.isEmpty()) {
    result.error = error;
    return result;
  }
  auto* qan_edge = EdgeFor(edge);
  if (qan_edge != nullptr) {
    ApplyEdgePresentation(*qan_edge, candidate);
  }
  edge_candidate_[MakeEdgeKey(edge)] = candidate;
  applied_.edges.push_back(edge);
  AttachLiveVisuals();
  result.succeeded = true;
  return result;
}

auto AlcedoQanGraph::RemoveProjectedEdge(const EditorNodeEdgeProjection& edge)
    -> AlcedoQanGraphApplyResult {
  AlcedoQanGraphApplyResult result;
  if (graph_.isNull()) {
    result.error = QStringLiteral("AlcedoQanGraph has no Qan graph");
    return result;
  }
  if (EdgeFor(edge) == nullptr) {
    result.succeeded = true;
    return result;
  }
  const auto error = RemoveEdgeVisual(edge);
  if (!error.isEmpty()) {
    result.error = error;
    return result;
  }
  EraseAppliedEdge(edge);
  result.succeeded = true;
  return result;
}

auto AlcedoQanGraph::RestoreNodePorts(const NodeId& node_id, qan::Node& node,
                                      const std::vector<PortVisualSpec>& ports) -> QString {
  QString error;
  for (const auto& spec : ports) {
    const auto& node_ports = ports_by_node_[node_id];
    const auto& map        = spec.is_input ? node_ports.inputs : node_ports.outputs;
    const auto  found      = map.find(spec.port_id);
    if (found != map.end() && !found->second.isNull()) {
      continue;
    }
    if (InsertPort(node, node_id, spec.port_id, spec.is_input) == nullptr) {
      if (!error.isEmpty()) {
        error += QStringLiteral("; ");
      }
      error += QStringLiteral("Qan port restoration failed");
    }
  }
  return error;
}

auto AlcedoQanGraph::RemoveNodeVisual(const NodeId& node_id, NodeVisualState* removed_state)
    -> QString {
  const auto found = node_by_id_.find(node_id);
  if (found == node_by_id_.end() || found->second.isNull()) {
    return QStringLiteral("Qan node is missing");
  }
  auto* const                 qan_node = found->second.data();
  const auto                  ports_it = ports_by_node_.find(node_id);
  std::vector<PortVisualSpec> ports;
  if (ports_it != ports_by_node_.end()) {
    ports.reserve(ports_it->second.inputs.size() + ports_it->second.outputs.size());
    for (const auto& [port_id, port] : ports_it->second.inputs) {
      if (!port.isNull()) {
        ports.push_back(PortVisualSpec{port_id, true});
      }
    }
    for (const auto& [port_id, port] : ports_it->second.outputs) {
      if (!port.isNull()) {
        ports.push_back(PortVisualSpec{port_id, false});
      }
    }
  }

  if (removed_state != nullptr) {
    const auto* projection = NodeProjection(node_id);
    if (projection != nullptr) {
      removed_state->projection = *projection;
    }
    const auto applied_node =
        std::find_if(applied_.nodes.begin(), applied_.nodes.end(),
                     [&](const auto& item) { return item.node_id == node_id; });
    if (applied_node != applied_.nodes.end()) {
      removed_state->applied_index =
          static_cast<std::size_t>(std::distance(applied_.nodes.begin(), applied_node));
    }
    if (qan_node->getItem() != nullptr) {
      removed_state->position     = qan_node->getItem()->position();
      removed_state->has_position = true;
      const auto drawer           = qan_node->getItem()->property("drawerOpen");
      if (drawer.isValid()) {
        removed_state->drawer_open = drawer.toBool();
      }
    }
  }

  auto restore_ports = [&]() {
    const auto restore_error = RestoreNodePorts(node_id, *qan_node, ports);
    AttachLiveVisuals();
    return restore_error;
  };

  for (const auto& spec : ports) {
    auto&      node_ports = ports_by_node_[node_id];
    auto&      map        = spec.is_input ? node_ports.inputs : node_ports.outputs;
    const auto found_port = map.find(spec.port_id);
    if (found_port == map.end() || found_port->second.isNull()) {
      continue;
    }
    if (!RemoveQanPort(*graph_, *qan_node, *found_port->second)) {
      QString    error         = QStringLiteral("Qan port removal failed");
      const auto restore_error = restore_ports();
      if (!restore_error.isEmpty()) {
        error += QStringLiteral("; restoring ports failed: ") + restore_error;
      }
      return error;
    }
    map.erase(found_port);
  }

  const auto* raw_node = qan_node;
  if (!RemoveQanNode(*graph_, *qan_node)) {
    QString    error         = QStringLiteral("Qan node removal failed");
    const auto restore_error = restore_ports();
    if (!restore_error.isEmpty()) {
      error += QStringLiteral("; restoring ports failed: ") + restore_error;
    }
    AttachLiveVisuals();
    return error;
  }
  node_from_qan_.erase(raw_node);
  node_by_id_.erase(node_id);
  ports_by_node_.erase(node_id);
  FlushDeferredDeletes();
  return {};
}

void AlcedoQanGraph::DetachEdgeFromPorts(qan::Edge& edge) {
  auto* item = edge.getItem();
  if (item == nullptr) {
    return;
  }
  if (auto* source = qobject_cast<qan::PortItem*>(item->getSourceItem())) {
    source->getOutEdgeItems().removeAll(item);
  }
  if (auto* destination = qobject_cast<qan::PortItem*>(item->getDestinationItem())) {
    destination->getInEdgeItems().removeAll(item);
  }
}

void AlcedoQanGraph::RestoreEdgeToPorts(qan::Edge& edge) {
  auto* item = edge.getItem();
  if (item == nullptr) {
    return;
  }
  if (auto* source = qobject_cast<qan::PortItem*>(item->getSourceItem())) {
    source->addOutEdgeItem(*item);
  }
  if (auto* destination = qobject_cast<qan::PortItem*>(item->getDestinationItem())) {
    destination->addInEdgeItem(*item);
  }
}

auto AlcedoQanGraph::RemoveCreatedEdge(qan::Edge& edge) -> QString {
  if (graph_.isNull()) {
    return QStringLiteral("AlcedoQanGraph has no Qan graph");
  }
  DetachEdgeFromPorts(edge);
  HideDetachedVisual(edge.getItem());
  if (!RemoveQanEdge(*graph_, edge)) {
    RestoreEdgeToPorts(edge);
    return QStringLiteral("Qan edge removal failed");
  }
  FlushDeferredDeletes();
  return {};
}

auto AlcedoQanGraph::RemoveEdgeVisual(const EditorNodeEdgeProjection& edge, bool require_present)
    -> QString {
  const auto key = MakeEdgeKey(edge);
  const auto it  = edge_by_key_.find(key);
  if (it == edge_by_key_.end()) {
    return require_present ? QStringLiteral("Qan edge is missing") : QString();
  }
  if (it->second.isNull()) {
    return QStringLiteral("Qan edge is missing");
  }
  auto* const qan_edge = it->second.data();
  DetachEdgeFromPorts(*qan_edge);
  HideDetachedVisual(qan_edge->getItem());
  if (!RemoveQanEdge(*graph_, *qan_edge)) {
    RestoreEdgeToPorts(*qan_edge);
    return QStringLiteral("Qan edge removal failed");
  }
  edge_by_key_.erase(it);
  edge_candidate_.erase(key);
  FlushDeferredDeletes();
  return {};
}

void AlcedoQanGraph::EraseAppliedNode(const NodeId& node_id) {
  applied_.nodes.erase(std::remove_if(applied_.nodes.begin(), applied_.nodes.end(),
                                      [&](const auto& node) { return node.node_id == node_id; }),
                       applied_.nodes.end());
}

void AlcedoQanGraph::EraseAppliedEdge(const EditorNodeEdgeProjection& edge) {
  const auto key = MakeEdgeKey(edge);
  applied_.edges.erase(std::remove_if(applied_.edges.begin(), applied_.edges.end(),
                                      [&](const auto& item) { return MakeEdgeKey(item) == key; }),
                       applied_.edges.end());
}

auto AlcedoQanGraph::ApplyMutation(const alcedo::EditorNodeGraphDraftMutation& mutation)
    -> AlcedoQanGraphApplyResult {
  AlcedoQanGraphApplyResult result;
  if (!mutation.succeeded) {
    result.error = QString::fromStdString(mutation.error);
    return result;
  }
  if (mutation.no_op) {
    result.succeeded = true;
    return result;
  }
  if (graph_.isNull() || !has_projection_) {
    result.error = QStringLiteral("AlcedoQanGraph has no complete projection");
    return result;
  }

  struct RemovedEdgeState {
    EditorNodeEdgeProjection edge;
    bool                     candidate = false;
  };
  std::vector<NodeVisualState>  removed_nodes;
  std::vector<RemovedEdgeState> removed_edges;
  removed_nodes.reserve(mutation.removed_node_ids.size());
  removed_edges.reserve(mutation.removed_edges.size());

  for (const auto& node_id : mutation.removed_node_ids) {
    const auto* projection = NodeProjection(node_id);
    if (projection == nullptr || NodeFor(node_id) == nullptr) {
      result.error = QStringLiteral("Qan mutation references a missing node");
      return result;
    }
    NodeVisualState state;
    state.projection = *projection;
    const auto applied_node =
        std::find_if(applied_.nodes.begin(), applied_.nodes.end(),
                     [&](const auto& item) { return item.node_id == node_id; });
    if (applied_node != applied_.nodes.end()) {
      state.applied_index =
          static_cast<std::size_t>(std::distance(applied_.nodes.begin(), applied_node));
    }
    if (auto* node = NodeFor(node_id); node != nullptr && node->getItem() != nullptr) {
      state.position     = node->getItem()->position();
      state.has_position = true;
      const auto drawer  = node->getItem()->property("drawerOpen");
      if (drawer.isValid()) {
        state.drawer_open = drawer.toBool();
      }
    }
    removed_nodes.push_back(std::move(state));
  }
  for (const auto& edge : mutation.removed_edges) {
    if (EdgeFor(edge) == nullptr) {
      result.error = QStringLiteral("Qan mutation references a missing edge");
      return result;
    }
    const auto key = MakeEdgeKey(edge);
    const auto it  = edge_candidate_.find(key);
    removed_edges.push_back(RemovedEdgeState{edge, it != edge_candidate_.end() && it->second});
  }
  for (const auto& node : mutation.inserted_nodes) {
    if (NodeFor(node.node_id) != nullptr) {
      result.error = QStringLiteral("Qan mutation inserts an existing node");
      return result;
    }
  }
  for (const auto& edge : mutation.inserted_edges) {
    if (EdgeFor(edge) != nullptr) {
      result.error = QStringLiteral("Qan mutation inserts an existing edge");
      return result;
    }
  }

  const NodeId                          prior_selection = product_selected_node_id_;
  std::vector<RemovedEdgeState>         completed_removed_edges;
  std::vector<NodeVisualState>          completed_removed_nodes;
  std::vector<NodeId>                   completed_inserted_nodes;
  std::vector<EditorNodeEdgeProjection> completed_inserted_edges;

  auto append_reversal_error = [](QString* all_errors, const QString& error) {
    if (error.isEmpty()) {
      return;
    }
    if (!all_errors->isEmpty()) {
      *all_errors += QStringLiteral("; ");
    }
    *all_errors += error;
  };
  auto apply_prior_selection = [&]() {
    if (prior_selection.Empty() || NodeFor(prior_selection) == nullptr) {
      ApplyProductSelection(std::nullopt);
      return;
    }
    ApplyProductSelection(prior_selection);
  };
  auto fail = [&](QString original_error) {
    QString reversal_errors;
    for (auto it = completed_inserted_edges.rbegin(); it != completed_inserted_edges.rend(); ++it) {
      append_reversal_error(&reversal_errors, RemoveEdgeVisual(*it));
    }
    for (auto it = completed_inserted_nodes.rbegin(); it != completed_inserted_nodes.rend(); ++it) {
      append_reversal_error(&reversal_errors, RemoveNodeVisual(*it));
    }
    for (auto it = completed_removed_nodes.rbegin(); it != completed_removed_nodes.rend(); ++it) {
      append_reversal_error(&reversal_errors,
                            InsertNodeVisual(it->projection, applied_.session_generation, &*it));
    }
    for (auto it = completed_removed_edges.rbegin(); it != completed_removed_edges.rend(); ++it) {
      append_reversal_error(&reversal_errors, InsertEdgeVisual(it->edge, it->candidate));
    }
    ClearDrawerConnections();
    BindDrawerSignals();
    AttachLiveVisuals();
    apply_prior_selection();
    result.error = std::move(original_error);
    if (!reversal_errors.isEmpty()) {
      result.error += QStringLiteral("; visual reversal failed: ") + reversal_errors;
    }
    result.succeeded = false;
    return result;
  };

  for (const auto& edge : removed_edges) {
    const auto error = RemoveEdgeVisual(edge.edge);
    if (!error.isEmpty()) {
      return fail(error);
    }
    completed_removed_edges.push_back(edge);
  }
  for (const auto& state : removed_nodes) {
    const auto error = RemoveNodeVisual(state.projection.node_id);
    if (!error.isEmpty()) {
      return fail(error);
    }
    completed_removed_nodes.push_back(state);
  }
  for (const auto& node : mutation.inserted_nodes) {
    const auto error = InsertNodeVisual(node, applied_.session_generation);
    if (!error.isEmpty()) {
      return fail(error);
    }
    completed_inserted_nodes.push_back(node.node_id);
  }
  for (const auto& edge : mutation.inserted_edges) {
    const auto error = InsertEdgeVisual(edge, true);
    if (!error.isEmpty()) {
      return fail(error);
    }
    completed_inserted_edges.push_back(edge);
  }

  for (const auto& edge : mutation.removed_edges) {
    EraseAppliedEdge(edge);
  }
  for (const auto& node_id : mutation.removed_node_ids) {
    EraseAppliedNode(node_id);
  }
  for (const auto& node : mutation.inserted_nodes) {
    applied_.nodes.push_back(node);
    BindDrawerSignal(NodeFor(node.node_id)->getItem(), node.node_id);
  }
  for (const auto& edge : mutation.inserted_edges) {
    applied_.edges.push_back(edge);
  }
  AttachLiveVisuals();
  apply_prior_selection();
  result.succeeded = true;
  return result;
}

void AlcedoQanGraph::PromoteCandidatePresentation() {
  for (auto& [key, candidate] : edge_candidate_) {
    candidate = false;
    auto it   = edge_by_key_.find(key);
    if (it != edge_by_key_.end() && !it->second.isNull()) {
      ApplyEdgePresentation(*it->second, false);
    }
  }
}

auto AlcedoQanGraph::PromoteCommittedSnapshot(const EditorNodeGraphSnapshot& snapshot)
    -> AlcedoQanGraphApplyResult {
  AlcedoQanGraphApplyResult result;
  if (!has_projection_ || graph_.isNull()) {
    result.error = QStringLiteral("AlcedoQanGraph has no Qan graph");
    return result;
  }
  if (snapshot.session_generation != applied_.session_generation) {
    result.error = QStringLiteral("snapshot session generation is stale");
    return result;
  }
  if (snapshot.nodes.size() != node_by_id_.size() || snapshot.edges.size() != edge_by_key_.size()) {
    result.error = QStringLiteral("committed snapshot does not match live Qan identities");
    return result;
  }
  for (const auto& node : snapshot.nodes) {
    if (NodeFor(node.node_id) == nullptr) {
      result.error = QStringLiteral("committed snapshot does not match live Qan identities");
      return result;
    }
  }
  for (const auto& edge : snapshot.edges) {
    if (EdgeFor(edge) == nullptr) {
      result.error = QStringLiteral("committed snapshot does not match live Qan identities");
      return result;
    }
  }
  for (const auto& node : snapshot.nodes) {
    auto* qan_node = NodeFor(node.node_id);
    ApplyNodePresentation(*qan_node, node);
  }
  PromoteCandidatePresentation();
  applied_         = snapshot;
  has_projection_  = true;
  result.succeeded = true;
  return result;
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

void AlcedoQanGraph::ApplyEdgePresentation(qan::Edge& qan_edge, bool candidate) {
  auto* item = qan_edge.getItem();
  if (item == nullptr) {
    return;
  }
  const auto color = candidate ? AppTheme::Instance().graphCandidateEdgeColor()
                               : AppTheme::Instance().graphEdgeColor();
  item->setProperty("candidate", candidate);
  if (auto* style = item->getStyle()) {
    style->setLineColor(color);
  }
  item->setSrcShape(qan::EdgeStyle::ArrowShape::None);
  item->setDstShape(qan::EdgeStyle::ArrowShape::None);
}

void AlcedoQanGraph::ApplyNodePresentation(qan::Node& qan_node, const EditorNodeProjection& node) {
  qan_node.setLabel(ToQString(node.display_name));
  auto* item = qan_node.getItem();
  if (item == nullptr) {
    return;
  }
  item->setResizable(false);
  item->setProperty("nodeKind", NodeKindKey(node.node_kind));
  item->setProperty("masks", MasksToVariant(node.masks));
}

auto AlcedoQanGraph::ComponentFor(EditorNodeKind kind) const -> QQmlComponent* {
  return delegate_library_.ComponentFor(kind);
}

auto AlcedoQanGraph::NodeKindKey(EditorNodeKind kind) -> QString {
  switch (kind) {
    case EditorNodeKind::Develop:
      return QStringLiteral("develop");
    case EditorNodeKind::Drt:
      return QStringLiteral("drt");
    case EditorNodeKind::ColorGrade:
      return QStringLiteral("colorGrade");
  }
  return QStringLiteral("colorGrade");
}

auto AlcedoQanGraph::SourceKindKey(MaskSourceKind kind) -> QString {
  switch (kind) {
    case MaskSourceKind::Brush:
      return QStringLiteral("brush");
    case MaskSourceKind::Radial:
      return QStringLiteral("radial");
    case MaskSourceKind::LinearGradient:
      return QStringLiteral("linearGradient");
  }
  return QStringLiteral("radial");
}

auto AlcedoQanGraph::MasksToVariant(const std::vector<EditorNodeMaskProjection>& masks)
    -> QVariantList {
  QVariantList list;
  list.reserve(static_cast<qsizetype>(masks.size()));
  for (const auto& mask : masks) {
    QVariantMap row;
    row.insert(QStringLiteral("maskId"), ToQString(mask.mask_id.Value()));
    row.insert(QStringLiteral("sourceKind"), SourceKindKey(mask.source_kind));
    list.push_back(row);
  }
  return list;
}

void AlcedoQanGraph::ConfigureGraphPolicy() {
  if (graph_.isNull()) {
    return;
  }
  graph_->setMultipleSelectionEnabled(false);
  graph_->setConnectorCreateDefaultEdge(false);
  graph_->setConnectorEnabled(true);
  connect(graph_.data(), &qan::Graph::connectorChanged, this, &AlcedoQanGraph::ConfigureConnector,
          Qt::UniqueConnection);
  connect(graph_.data(), &qan::Graph::connectorRequestEdgeCreation, this,
          &AlcedoQanGraph::OnConnectorRequestEdgeCreation, Qt::UniqueConnection);
  ConfigureConnector();
}

void AlcedoQanGraph::ConfigureConnector() {
  if (graph_.isNull()) {
    return;
  }
  const auto& theme = AppTheme::Instance();
  graph_->setConnectorCreateDefaultEdge(false);
  graph_->setConnectorEnabled(true);
  graph_->setConnectorEdgeColor(theme.graphCandidateEdgeColor());
  graph_->setConnectorColor(theme.graphPortBorderColor());
  if (auto* connector = graph_->getConnector()) {
    connector->setProperty("createDefaultEdge", false);
    if (connector->getSourceNode() == nullptr && connector->getSourcePort() == nullptr) {
      connector->setVisible(false);
    }
  }
  ApplyConnectablePolicy();
}

void AlcedoQanGraph::ApplyConnectablePolicy() {
  if (graph_.isNull() || rebuild_in_progress_) {
    return;
  }
  for (const auto& [node_id, node] : node_by_id_) {
    if (node.isNull() || node->getItem() == nullptr) {
      continue;
    }
    const auto* projection = NodeProjection(node_id);
    if (projection == nullptr) {
      continue;
    }
    auto connectable = qan::NodeItem::Connectable::UnConnectable;
    if (projection->node_kind == EditorNodeKind::Drt) {
      connectable = qan::NodeItem::Connectable::InConnectable;
    } else if (projection->node_kind == EditorNodeKind::ColorGrade) {
      connectable = qan::NodeItem::Connectable::Connectable;
    } else if (projection->node_kind == EditorNodeKind::Develop) {
      connectable = qan::NodeItem::Connectable::OutConnectable;
    }
    node->getItem()->setConnectable(connectable);
  }

  auto* connector = graph_->getConnector();
  if (connector == nullptr) {
    return;
  }
  const auto* selected = NodeProjection(product_selected_node_id_);
  auto*       source   = NodeFor(product_selected_node_id_);
  const bool  can_start =
      selected != nullptr && selected->node_kind != EditorNodeKind::Drt && source != nullptr;
  if (!can_start) {
    connector->setSourcePort(nullptr);
    connector->setSourceNode(nullptr);
    connector->setVisible(false);
    return;
  }
  auto* output = OutputPortFor(product_selected_node_id_, PortId{"image"});
  if (output != nullptr) {
    connector->setSourcePort(output);
  } else {
    connector->setSourceNode(source);
  }
  connector->setEnabled(true);
  connector->setVisible(true);
}

void AlcedoQanGraph::hideConnectorPreview() {
  if (graph_.isNull()) {
    return;
  }
  auto* connector = graph_->getConnector();
  if (connector == nullptr) {
    return;
  }
  if (auto* edge = connector->getEdgeItem()) {
    edge->setVisible(false);
  }
  if (auto* item = connector->getConnectorItem()) {
    item->setProperty("state", QStringLiteral("NORMAL"));
  }
}

void AlcedoQanGraph::OnConnectorRequestEdgeCreation(qan::Node* src, QObject* dst,
                                                    qan::PortItem* /*src_port*/,
                                                    qan::PortItem* dst_port) {
  hideConnectorPreview();
  const auto source_id = LiveNodeId(src);
  if (!source_id.has_value()) {
    emit ConnectorRequestRejected(
        QStringLiteral("That graph item is no longer part of the current Version"));
    return;
  }
  qan::Node* destination_node = qobject_cast<qan::Node*>(dst);
  if (destination_node == nullptr && dst_port != nullptr) {
    destination_node = dst_port->getNode();
  }
  if (destination_node == nullptr) {
    if (auto* item = qobject_cast<qan::NodeItem*>(dst)) {
      destination_node = item->getNode();
    }
  }
  const auto destination_id = LiveNodeId(destination_node);
  if (!destination_id.has_value()) {
    emit ConnectorRequestRejected(
        QStringLiteral("That graph item is no longer part of the current Version"));
    return;
  }
  const bool destination_is_output =
      dst_port != nullptr && dst_port->getType() == qan::PortItem::Type::Out;
  emit ConnectorMoveRequested(ToQString(source_id->Value()), ToQString(destination_id->Value()),
                              destination_is_output);
}

void AlcedoQanGraph::ClearDrawerConnections() {
  for (auto& connection : drawer_connections_) {
    QObject::disconnect(connection);
  }
  drawer_connections_.clear();
}

void AlcedoQanGraph::BindDrawerSignal(QQuickItem* item, const NodeId& /*node_id*/) {
  if (item == nullptr) {
    return;
  }
  const auto* meta         = item->metaObject();
  const int   signal_index = meta->indexOfSignal("drawerOpenChanged()");
  if (signal_index < 0) {
    return;
  }
  drawer_connections_.push_back(
      QObject::connect(item, SIGNAL(drawerOpenChanged()), this, SLOT(OnDrawerOpenChanged())));
}

void AlcedoQanGraph::OnDrawerOpenChanged() {
  auto* item = qobject_cast<QQuickItem*>(sender());
  if (item == nullptr) {
    return;
  }
  for (const auto& [node_id, node] : node_by_id_) {
    if (node.isNull() || node->getItem() != item) {
      continue;
    }
    emit NodeDrawerOpenChanged(ToQString(node_id.Value()), item->property("drawerOpen").toBool());
    return;
  }
}

void AlcedoQanGraph::BindDrawerSignals() {
  ClearDrawerConnections();
  for (const auto& [node_id, node] : node_by_id_) {
    if (node.isNull() || node->getItem() == nullptr) {
      continue;
    }
    BindDrawerSignal(node->getItem(), node_id);
  }
}

void AlcedoQanGraph::ApplyProductSelection(const std::optional<NodeId>& node_id) {
  if (graph_.isNull() || rebuild_in_progress_) {
    return;
  }
  product_selected_node_id_ = node_id.value_or(NodeId{});
  graph_->clearSelection();
  if (node_id.has_value() && !node_id->Empty()) {
    auto* node = NodeFor(*node_id);
    if (node != nullptr) {
      graph_->setNodeSelected(node, true);
    }
  }
  ApplyConnectablePolicy();
}

void AlcedoQanGraph::SetNodeItemPosition(const NodeId& node_id, QPointF position) {
  auto* node = NodeFor(node_id);
  if (node == nullptr || node->getItem() == nullptr) {
    return;
  }
  node->getItem()->setPosition(position);
}

auto AlcedoQanGraph::NodeItemPosition(const NodeId& node_id) const -> std::optional<QPointF> {
  auto* node = NodeFor(node_id);
  if (node == nullptr || node->getItem() == nullptr) {
    return std::nullopt;
  }
  return node->getItem()->position();
}

void AlcedoQanGraph::SetDrawerOpen(const NodeId& node_id, bool open) {
  auto* node = NodeFor(node_id);
  if (node == nullptr || node->getItem() == nullptr) {
    return;
  }
  if (!node->getItem()->property("drawerOpen").isValid()) {
    return;
  }
  node->getItem()->setProperty("drawerOpen", open);
}

auto AlcedoQanGraph::DrawerOpen(const NodeId& node_id) const -> bool {
  auto* node = NodeFor(node_id);
  if (node == nullptr || node->getItem() == nullptr) {
    return true;
  }
  const auto value = node->getItem()->property("drawerOpen");
  if (!value.isValid()) {
    return true;
  }
  return value.toBool();
}

auto AlcedoQanGraph::liveNodeId(QObject* node) const -> QString {
  const auto id = LiveNodeId(qobject_cast<qan::Node*>(node));
  return id.has_value() ? ToQString(id->Value()) : QString();
}

void AlcedoQanGraph::setNodePosition(const QString& node_id, qreal x, qreal y) {
  SetNodeItemPosition(NodeId{node_id.toStdString()}, QPointF(x, y));
}

auto AlcedoQanGraph::nodePosition(const QString& node_id) const -> QPointF {
  return NodeItemPosition(NodeId{node_id.toStdString()}).value_or(QPointF());
}

void AlcedoQanGraph::setDrawerOpen(const QString& node_id, bool open) {
  SetDrawerOpen(NodeId{node_id.toStdString()}, open);
}

auto AlcedoQanGraph::drawerOpen(const QString& node_id) const -> bool {
  return DrawerOpen(NodeId{node_id.toStdString()});
}

void AlcedoQanGraph::applyProductSelection(const QString& node_id) {
  if (node_id.isEmpty()) {
    ApplyProductSelection(std::nullopt);
    return;
  }
  ApplyProductSelection(NodeId{node_id.toStdString()});
}

}  // namespace alcedo::ui
