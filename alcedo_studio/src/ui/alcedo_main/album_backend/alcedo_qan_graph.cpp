//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/alcedo_qan_graph.hpp"

#include <QDebug>
#include <QMetaMethod>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QVariant>
#include <QVariantMap>
#include <cstddef>
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

}  // namespace

AlcedoQanGraph::AlcedoQanGraph(QObject* parent) : QObject(parent) {}

AlcedoQanGraph::~AlcedoQanGraph() {
  rebuild_in_progress_ = true;
  ClearDrawerConnections();
  ClearIdentityMaps();
  DropCachedDelegates();
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
  DropCachedDelegates();
  rebuild_in_progress_ = false;
  graph_               = graph;
  if (!graph_.isNull()) {
    connect(graph_.data(), &QObject::destroyed, this, &AlcedoQanGraph::OnGraphDestroyed);
    ConfigureGraphPolicy();
  }
  emit GraphChanged();
}

void AlcedoQanGraph::set_color_grade_delegate_url(const QUrl& url) {
  if (color_grade_delegate_url_ == url) {
    return;
  }
  color_grade_delegate_url_ = url;
  DropCachedDelegates();
  emit DelegatesChanged();
}

auto AlcedoQanGraph::color_grade_delegate_url() const -> QUrl { return color_grade_delegate_url_; }

void AlcedoQanGraph::set_endpoint_delegate_url(const QUrl& url) {
  if (endpoint_delegate_url_ == url) {
    return;
  }
  endpoint_delegate_url_ = url;
  DropCachedDelegates();
  emit DelegatesChanged();
}

auto AlcedoQanGraph::endpoint_delegate_url() const -> QUrl { return endpoint_delegate_url_; }

void AlcedoQanGraph::set_port_delegate_url(const QUrl& url) {
  if (port_delegate_url_ == url) {
    return;
  }
  port_delegate_url_ = url;
  DropCachedDelegates();
  emit DelegatesChanged();
}

auto AlcedoQanGraph::port_delegate_url() const -> QUrl { return port_delegate_url_; }

void AlcedoQanGraph::set_port_dock_delegate_url(const QUrl& url) {
  if (port_dock_delegate_url_ == url) {
    return;
  }
  port_dock_delegate_url_ = url;
  DropCachedDelegates();
  emit DelegatesChanged();
}

auto AlcedoQanGraph::port_dock_delegate_url() const -> QUrl { return port_dock_delegate_url_; }

void AlcedoQanGraph::set_edge_delegate_url(const QUrl& url) {
  if (edge_delegate_url_ == url) {
    return;
  }
  edge_delegate_url_ = url;
  DropCachedDelegates();
  emit DelegatesChanged();
}

auto AlcedoQanGraph::edge_delegate_url() const -> QUrl { return edge_delegate_url_; }

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

auto AlcedoQanGraph::InsertQanNode(qan::Graph& graph, const EditorNodeProjection& node)
    -> qan::Node* {
  auto* component = ComponentFor(node.node_kind);
  if (component == nullptr) {
    return nullptr;
  }
  return graph.insertNode(component);
}

void AlcedoQanGraph::ClearIdentityMaps() {
  ClearDrawerConnections();
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
  DropCachedDelegates();
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
      HideDetachedVisual(edge->getItem());
      graph_->removeEdge(edge.data(), true);
    }
  }
  for (const auto& node : nodes) {
    if (!node.isNull()) {
      HideDetachedVisual(node->getItem());
      graph_->removeNode(node.data(), true);
    }
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
    node_projections_[node.node_id] = node;
  }
  applied_         = snapshot;
  has_projection_  = true;
  BindDrawerSignals();
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
    BindDrawerSignals();
    ApplyConnectablePolicy();
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
      BindDrawerSignals();
    } else {
      error += QStringLiteral("; restoring the prior Qan projection failed: ") + restore_error;
    }
  }
  rebuild_in_progress_ = false;
  ApplyConnectablePolicy();
  result.error         = error;
  return result;
}

auto AlcedoQanGraph::InsertTopology(const EditorNodeGraphSnapshot& snapshot) -> QString {
  if (graph_.isNull()) {
    return QStringLiteral("AlcedoQanGraph has no Qan graph");
  }
  const auto delegate_error = EnsureDelegates();
  if (!delegate_error.isEmpty()) {
    return delegate_error;
  }
  for (const auto& node : snapshot.nodes) {
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
      graph_->removeNode(qan_node, true);
      return QStringLiteral("Qan node is missing a visual item");
    }
    ApplyNodePresentation(*qan_node, node);
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
  AttachLiveVisuals();
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
  auto* qan_edge = graph_->insertEdge(source, dest, edge_component_.get());
  if (qan_edge == nullptr || qan_edge->getItem() == nullptr) {
    return QStringLiteral("Qan edge creation failed");
  }
  if (auto* item = qan_edge->getItem()) {
    item->setSrcShape(qan::EdgeStyle::ArrowShape::None);
    item->setDstShape(qan::EdgeStyle::ArrowShape::None);
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

auto AlcedoQanGraph::EnsureDelegates() -> QString {
  if (graph_.isNull()) {
    return QStringLiteral("AlcedoQanGraph has no Qan graph");
  }
  auto* engine = qmlEngine(graph_.data());
  if (engine == nullptr) {
    return QStringLiteral("Qan graph has no QML engine");
  }
  if (delegate_engine_.data() != engine) {
    DropCachedDelegates();
    delegate_engine_ = engine;
  }
  if (!color_grade_component_) {
    auto loaded = LoadComponent(color_grade_delegate_url_,
                                QStringLiteral("Alcedo color-grade node delegate"));
    if (!loaded.component) {
      return loaded.error;
    }
    color_grade_component_ = std::move(loaded.component);
  }
  if (!endpoint_component_) {
    auto loaded =
        LoadComponent(endpoint_delegate_url_, QStringLiteral("Alcedo endpoint node delegate"));
    if (!loaded.component) {
      return loaded.error;
    }
    endpoint_component_ = std::move(loaded.component);
  }
  if (!edge_component_) {
    auto loaded = LoadComponent(edge_delegate_url_, QStringLiteral("Alcedo edge delegate"));
    if (!loaded.component) {
      return loaded.error;
    }
    edge_component_ = std::move(loaded.component);
  }
  const auto port_error = InstallPortDelegate();
  if (!port_error.isEmpty()) {
    return port_error;
  }
  return InstallPortDockDelegate();
}

auto AlcedoQanGraph::LoadComponent(const QUrl& url, const QString& role) -> LoadedComponent {
  LoadedComponent loaded;
  if (url.isEmpty()) {
    loaded.error = role + QStringLiteral(" URL is empty");
    return loaded;
  }
  if (graph_.isNull()) {
    loaded.error = QStringLiteral("AlcedoQanGraph has no Qan graph");
    return loaded;
  }
  auto* engine = qmlEngine(graph_.data());
  if (engine == nullptr) {
    loaded.error = QStringLiteral("Qan graph has no QML engine");
    return loaded;
  }
  auto component = std::make_unique<QQmlComponent>(engine, url, QQmlComponent::PreferSynchronous);
  if (component->isError() || !component->isReady()) {
    loaded.error = role + QStringLiteral(" failed to load");
    if (!component->errorString().isEmpty()) {
      loaded.error += QStringLiteral(": ") + component->errorString().trimmed();
    }
    return loaded;
  }
  loaded.component = std::move(component);
  return loaded;
}

void AlcedoQanGraph::DropCachedDelegates() {
  color_grade_component_.reset();
  endpoint_component_.reset();
  edge_component_.reset();
  delegate_engine_.clear();
  port_delegate_graph_.clear();
  port_dock_delegate_graph_.clear();
}

auto AlcedoQanGraph::InstallPortDelegate() -> QString {
  if (graph_.isNull()) {
    return QStringLiteral("AlcedoQanGraph has no Qan graph");
  }
  if (port_delegate_graph_.data() == graph_.data()) {
    return {};
  }
  auto loaded = LoadComponent(port_delegate_url_, QStringLiteral("Alcedo port delegate"));
  if (!loaded.component) {
    return loaded.error;
  }
  graph_->setProperty("portDelegate", QVariant::fromValue(loaded.component.release()));
  port_delegate_graph_ = graph_;
  return {};
}

auto AlcedoQanGraph::InstallPortDockDelegate() -> QString {
  if (graph_.isNull()) {
    return QStringLiteral("AlcedoQanGraph has no Qan graph");
  }
  if (port_dock_delegate_graph_.data() == graph_.data()) {
    return {};
  }
  auto loaded =
      LoadComponent(port_dock_delegate_url_, QStringLiteral("Alcedo port dock delegate"));
  if (!loaded.component) {
    return loaded.error;
  }
  graph_->setProperty("horizontalDockDelegate",
                      QVariant::fromValue(loaded.component.release()));
  port_dock_delegate_graph_ = graph_;
  return {};
}

void AlcedoQanGraph::InstallInvisibleSelectionDelegate() {
  if (graph_.isNull()) {
    return;
  }
  auto* engine = qmlEngine(graph_.data());
  if (engine == nullptr) {
    return;
  }
  // Node delegates paint their own selection outline; the QuickQanava
  // selection item must exist (qan::NodeItem expects one) but render nothing.
  auto component = std::make_unique<QQmlComponent>(engine);
  component->setData(QByteArrayLiteral("import QtQuick\nItem {}\n"), QUrl());
  if (!component->isReady()) {
    qWarning() << "AlcedoQanGraph: failed to create invisible selection delegate:"
               << component->errorString();
    return;
  }
  graph_->setProperty("selectionDelegate", QVariant::fromValue(component.release()));
}

auto AlcedoQanGraph::ComponentFor(EditorNodeKind kind) const -> QQmlComponent* {
  if (kind == EditorNodeKind::ColorGrade) {
    return color_grade_component_.get();
  }
  return endpoint_component_.get();
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
  InstallInvisibleSelectionDelegate();
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
      connectable = node_id == product_selected_node_id_
                        ? qan::NodeItem::Connectable::OutConnectable
                        : qan::NodeItem::Connectable::InConnectable;
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
      selected != nullptr && selected->node_kind == EditorNodeKind::ColorGrade && source != nullptr;
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
