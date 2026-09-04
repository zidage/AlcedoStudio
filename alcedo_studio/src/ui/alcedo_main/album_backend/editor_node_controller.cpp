//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_node_controller.hpp"

#include <qqml.h>

#include <QMetaObject>
#include <QScopeGuard>
#include <QUuid>
#include <algorithm>
#include <utility>

#include "ui/alcedo_main/album_backend/alcedo_qan_graph.hpp"
#include "ui/alcedo_main/album_backend/editor_node_layout_store.hpp"
#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"

namespace alcedo::ui {
namespace {

auto SameTopology(const EditorNodeGraphSnapshot& lhs, const EditorNodeGraphSnapshot& rhs) -> bool {
  if (lhs.nodes.size() != rhs.nodes.size() || lhs.edges.size() != rhs.edges.size()) {
    return false;
  }
  for (std::size_t i = 0; i < lhs.nodes.size(); ++i) {
    if (lhs.nodes[i].node_id != rhs.nodes[i].node_id ||
        lhs.nodes[i].node_kind != rhs.nodes[i].node_kind) {
      return false;
    }
  }
  for (std::size_t i = 0; i < lhs.edges.size(); ++i) {
    if (lhs.edges[i].source_node_id != rhs.edges[i].source_node_id ||
        lhs.edges[i].source_port_id != rhs.edges[i].source_port_id ||
        lhs.edges[i].destination_node_id != rhs.edges[i].destination_node_id ||
        lhs.edges[i].destination_port_id != rhs.edges[i].destination_port_id) {
      return false;
    }
  }
  return true;
}

}  // namespace

EditorNodeController::EditorNodeController(QObject* parent) : QObject(parent) {}

EditorNodeController::~EditorNodeController() {
  if (graph_adapter_connection_) {
    QObject::disconnect(graph_adapter_connection_);
    graph_adapter_connection_ = {};
  }
  DisconnectSession();
}

auto EditorNodeController::editor_session_object() const -> QObject* { return session_.data(); }

auto EditorNodeController::session() const -> EditorSessionController* { return session_.data(); }

void EditorNodeController::DisconnectSession() {
  if (state_connection_) {
    QObject::disconnect(state_connection_);
    state_connection_ = {};
  }
  if (history_connection_) {
    QObject::disconnect(history_connection_);
    history_connection_ = {};
  }
  if (availability_connection_) {
    QObject::disconnect(availability_connection_);
    availability_connection_ = {};
  }
  session_.clear();
}

void EditorNodeController::set_editor_session(QObject* session) {
  auto* typed = qobject_cast<EditorSessionController*>(session);
  if (typed == session_.data()) {
    return;
  }
  DisconnectSession();
  session_ = typed;
  if (session_ != nullptr) {
    state_connection_   = connect(session_.data(), &EditorSessionController::StateChanged, this,
                                  &EditorNodeController::OnSessionChanged);
    history_connection_ = connect(session_.data(), &EditorSessionController::HistoryChanged, this,
                                  &EditorNodeController::OnSessionChanged);
    availability_connection_ =
        connect(session_.data(), &EditorSessionController::ActionAvailabilityChanged, this,
                &EditorNodeController::ActionAvailabilityChanged);
  }
  emit EditorSessionChanged();
  OnSessionChanged();
}

void EditorNodeController::SetLayoutIdentity(quint64 element_id, quint64 image_id,
                                             QString version_id) {
  element_id_ = element_id;
  image_id_   = image_id;
  version_id_ = std::move(version_id);
  emit SnapshotChanged();
}

auto EditorNodeController::BoundSessionGeneration() const -> std::optional<std::uint64_t> {
  if (session_ == nullptr) {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(session_->session_generation());
}

void EditorNodeController::SetLastError(QString error) {
  if (last_error_ == error) {
    return;
  }
  last_error_ = std::move(error);
  emit lastErrorChanged();
}

void EditorNodeController::ClearSnapshot() {
  snapshot_                  = {};
  has_snapshot_              = false;
  selected_node_id_          = {};
  selection_restore_node_id_ = {};
  session_generation_        = BoundSessionGeneration().value_or(0);
  projection_revision_       = 0;
  topology_revision_         = 0;
  emit SnapshotChanged();
  emit SelectionChanged();
  emit ActionAvailabilityChanged();
}

auto EditorNodeController::ContainsNode(const NodeId& node_id) const -> bool {
  if (!has_snapshot_ || node_id.Empty()) {
    return false;
  }
  for (const auto& node : snapshot_.nodes) {
    if (node.node_id == node_id) {
      return true;
    }
  }
  return false;
}

auto EditorNodeController::DefaultSelectedNodeId() const -> NodeId {
  if (!has_snapshot_) {
    return {};
  }
  for (const auto& node : snapshot_.nodes) {
    if (node.node_kind == EditorNodeKind::ColorGrade) {
      return node.node_id;
    }
  }
  for (const auto& node : snapshot_.nodes) {
    if (node.node_kind == EditorNodeKind::Drt) {
      return node.node_id;
    }
  }
  return snapshot_.nodes.front().node_id;
}

auto EditorNodeController::IndexOf(const NodeId& node_id) const -> int {
  if (!has_snapshot_) {
    return -1;
  }
  for (int i = 0; i < static_cast<int>(snapshot_.nodes.size()); ++i) {
    if (snapshot_.nodes[static_cast<std::size_t>(i)].node_id == node_id) {
      return i;
    }
  }
  return -1;
}

auto EditorNodeController::NodeFor(const NodeId& node_id) const -> const EditorNodeProjection* {
  if (!has_snapshot_) {
    return nullptr;
  }
  const auto it = std::find_if(snapshot_.nodes.begin(), snapshot_.nodes.end(),
                               [&](const auto& node) { return node.node_id == node_id; });
  return it == snapshot_.nodes.end() ? nullptr : &*it;
}

auto EditorNodeController::IsColorGrade(const NodeId& node_id) const -> bool {
  const auto* node = NodeFor(node_id);
  return node != nullptr && node->node_kind == EditorNodeKind::ColorGrade;
}

auto EditorNodeController::TopologyChanged(const EditorNodeGraphSnapshot& snapshot) const -> bool {
  if (!has_snapshot_) {
    return true;
  }
  return !SameTopology(snapshot_, snapshot);
}

void EditorNodeController::RestoreSelectionAfterSnapshot() {
  if (ContainsNode(selection_restore_node_id_)) {
    selected_node_id_          = selection_restore_node_id_;
    selection_restore_node_id_ = {};
    return;
  }
  if (ContainsNode(selected_node_id_)) {
    return;
  }
  if (!selected_node_id_.Empty()) {
    selection_restore_node_id_ = selected_node_id_;
  }
  selected_node_id_ = DefaultSelectedNodeId();
}

auto EditorNodeController::selected_node_id_string() const -> QString {
  return NodeIdToQString(selected_node_id_);
}

auto EditorNodeController::backbone_node_ids() const -> QStringList {
  QStringList ids;
  if (!has_snapshot_) {
    return ids;
  }
  ids.reserve(static_cast<qsizetype>(snapshot_.nodes.size()));
  for (const auto& node : snapshot_.nodes) {
    ids.push_back(NodeIdToQString(node.node_id));
  }
  return ids;
}

auto EditorNodeController::selected_node_name() const -> QString {
  const auto* node = NodeFor(selected_node_id_);
  return node == nullptr ? QString{} : QString::fromStdString(node->display_name);
}

auto EditorNodeController::can_add_color_grade() const -> bool {
  return has_snapshot_ && session_ != nullptr && session_->can_edit() && !command_active_;
}

auto EditorNodeController::can_rename_selected_color_grade() const -> bool {
  return can_add_color_grade() && IsColorGrade(selected_node_id_);
}

auto EditorNodeController::can_delete_selected_color_grade() const -> bool {
  return can_rename_selected_color_grade();
}

auto EditorNodeController::PublishSnapshot(EditorNodeGraphSnapshot snapshot) -> bool {
  const auto bound_generation = BoundSessionGeneration();
  if (bound_generation.has_value() && snapshot.session_generation != *bound_generation) {
    SetLastError(tr("The graph snapshot is from another editor session"));
    return false;
  }
  const bool assign_revisions =
      snapshot.topology_revision == 0 && snapshot.projection_revision == 0;
  if (!assign_revisions && has_snapshot_ && snapshot.session_generation == session_generation_) {
    if (snapshot.topology_revision < topology_revision_ ||
        (snapshot.topology_revision == topology_revision_ &&
         snapshot.projection_revision < projection_revision_)) {
      SetLastError(tr("The graph snapshot is older than the live projection"));
      return false;
    }
  }
  if (snapshot.nodes.empty()) {
    SetLastError(tr("The graph snapshot has no nodes"));
    return false;
  }

  const bool generation_changed =
      !has_snapshot_ || snapshot.session_generation != session_generation_;
  if (generation_changed) {
    selection_restore_node_id_ = {};
    session_generation_        = snapshot.session_generation;
    topology_revision_         = snapshot.topology_revision == 0 ? 1 : snapshot.topology_revision;
    projection_revision_ = snapshot.projection_revision == 0 ? 1 : snapshot.projection_revision;
  } else if (TopologyChanged(snapshot)) {
    topology_revision_   = std::max(topology_revision_ + 1, snapshot.topology_revision);
    projection_revision_ = std::max(projection_revision_ + 1, snapshot.projection_revision);
  } else {
    projection_revision_ = std::max(projection_revision_ + 1, snapshot.projection_revision);
  }
  snapshot.session_generation  = session_generation_;
  snapshot.topology_revision   = topology_revision_;
  snapshot.projection_revision = projection_revision_;
  snapshot_                    = std::move(snapshot);
  has_snapshot_                = true;
  RestoreSelectionAfterSnapshot();
  SetLastError({});
  emit SnapshotChanged();
  emit SelectionChanged();
  emit ActionAvailabilityChanged();
  QueueProjectionApply();
  return true;
}

auto EditorNodeController::PublishDocument(const PipelineDocument& document,
                                           std::uint64_t           session_generation) -> bool {
  const auto bound_generation = BoundSessionGeneration();
  if (bound_generation.has_value() && session_generation != *bound_generation) {
    SetLastError(tr("The graph snapshot is from another editor session"));
    return false;
  }
  try {
    auto built = EditorNodeGraphProjection::Build(document, session_generation, 0, 0);
    return PublishSnapshot(std::move(built));
  } catch (const std::exception& ex) {
    SetLastError(QString::fromUtf8(ex.what()));
    return false;
  }
}

bool EditorNodeController::applyToGraph(QObject* adapter) {
  auto* graph = qobject_cast<AlcedoQanGraph*>(adapter);
  if (graph == nullptr) {
    SetLastError(tr("The graph adapter is missing"));
    return false;
  }
  if (!has_snapshot_) {
    SetLastError(tr("The node graph has no snapshot"));
    return false;
  }
  const auto result = graph->ApplySnapshot(snapshot_);
  if (!result.succeeded) {
    SetLastError(result.error);
    return false;
  }
  graph->ApplyProductSelection(selected_node_id_);
  SetLastError({});
  return true;
}

auto EditorNodeController::graph_adapter_object() const -> QObject* {
  return graph_adapter_.data();
}

void EditorNodeController::set_graph_adapter(QObject* adapter) {
  auto* graph = qobject_cast<AlcedoQanGraph*>(adapter);
  if (graph_adapter_.data() == graph) {
    return;
  }
  if (graph_adapter_connection_) {
    QObject::disconnect(graph_adapter_connection_);
    graph_adapter_connection_ = {};
  }
  graph_adapter_ = graph;
  if (graph_adapter_ != nullptr) {
    graph_adapter_connection_ = connect(graph_adapter_.data(), &AlcedoQanGraph::GraphChanged, this,
                                        [this] { QueueProjectionApply(); });
  }
  emit GraphAdapterChanged();
  if (graph_adapter_ != nullptr && has_snapshot_) {
    ApplyBoundGraph();
  }
}

auto EditorNodeController::layout_store_object() const -> QObject* { return layout_store_.data(); }

void EditorNodeController::set_layout_store(QObject* store) {
  auto* layout = qobject_cast<EditorNodeLayoutStore*>(store);
  if (layout_store_.data() == layout) {
    return;
  }
  layout_store_ = layout;
  emit LayoutStoreChanged();
}

void EditorNodeController::ApplyBoundGraph() {
  if (graph_adapter_.isNull() || !has_snapshot_ || graph_adapter_->graph() == nullptr) {
    return;
  }
  if (!applyToGraph(graph_adapter_.data())) {
    return;
  }
  auto* layout = layout_store_.data();
  if (layout == nullptr) {
    return;
  }
  layout->ensureDefaultsFrom(this);
  for (const auto& node : snapshot_.nodes) {
    const auto id = NodeIdToQString(node.node_id);
    if (layout->hasNodePosition(id)) {
      const auto pos = layout->nodePosition(id);
      graph_adapter_->setNodePosition(id, pos.x(), pos.y());
    }
    graph_adapter_->setDrawerOpen(id, layout->drawerOpen(id));
  }
  graph_adapter_->applyProductSelection(selected_node_id_string());
}

void EditorNodeController::QueueProjectionApply() {
  if (projection_apply_queued_) {
    return;
  }
  projection_apply_queued_ = true;
  QMetaObject::invokeMethod(
      this,
      [this] {
        projection_apply_queued_ = false;
        ApplyBoundGraph();
      },
      Qt::QueuedConnection);
}

bool EditorNodeController::refreshFromSession() {
  if (session_ == nullptr) {
    ClearSnapshot();
    SetLastError(tr("No editor session is bound"));
    return false;
  }
  element_id_ = session_->element_id();
  image_id_   = session_->image_id();
  version_id_ = QString::fromStdString(session_->history_snapshot().active_version_id.ToString());
  const auto* document = session_->pipeline_document();
  if (document == nullptr) {
    ClearSnapshot();
    SetLastError({});
    emit SnapshotChanged();
    return false;
  }
  return PublishDocument(*document, static_cast<std::uint64_t>(session_->session_generation()));
}

void EditorNodeController::OnSessionChanged() { refreshFromSession(); }

void EditorNodeController::selectNode(const QString& node_id) {
  const auto id = NodeIdFromQString(node_id);
  if (!ContainsNode(id)) {
    SetLastError(tr("That node is not in the current graph"));
    return;
  }
  if (selected_node_id_ == id) {
    selection_restore_node_id_ = {};
    SetLastError({});
    return;
  }
  selected_node_id_          = id;
  selection_restore_node_id_ = {};
  SetLastError({});
  emit SelectionChanged();
  emit ActionAvailabilityChanged();
}

void EditorNodeController::SetCommandActive(bool active) {
  if (command_active_ == active) {
    return;
  }
  command_active_ = active;
  emit CommandStateChanged();
  emit ActionAvailabilityChanged();
}

auto EditorNodeController::ValidateCommandGeneration() -> bool {
  if (command_active_) {
    SetLastError(tr("Another node command is active"));
    return false;
  }
  if (session_ == nullptr || !has_snapshot_) {
    SetLastError(tr("No editable node graph is available"));
    return false;
  }
  const auto bound_generation = BoundSessionGeneration();
  if (!bound_generation.has_value() || session_generation_ != *bound_generation) {
    SetLastError(tr("The node command is from another editor session"));
    return false;
  }
  if (!session_->can_edit()) {
    SetLastError(tr("The node graph is not editable in the current session state"));
    return false;
  }
  return true;
}

bool EditorNodeController::addCleanColorGrade() {
  if (!ValidateCommandGeneration()) {
    return false;
  }
  NodeId    before_id{"drt"};
  const int selected_index = IndexOf(selected_node_id_);
  if (IsColorGrade(selected_node_id_) && selected_index >= 0 &&
      selected_index + 1 < static_cast<int>(snapshot_.nodes.size())) {
    before_id = snapshot_.nodes[static_cast<std::size_t>(selected_index + 1)].node_id;
  }
  const auto   uuid = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower().toStdString();
  const NodeId new_id{"grade." + uuid};

  SetCommandActive(true);
  const auto reset_active = qScopeGuard([this] { SetCommandActive(false); });
  const auto result       = session_->SubmitAddColorGrade(before_id, new_id);
  if (EditorSessionResultIsFailure(result.kind)) {
    SetLastError(QString::fromStdString(result.message));
    return false;
  }
  refreshFromSession();
  selectNode(NodeIdToQString(new_id));
  QueueProjectionApply();
  return true;
}

bool EditorNodeController::renameColorGrade(const QString& node_id, const QString& display_name) {
  if (!ValidateCommandGeneration()) {
    return false;
  }
  const auto id = NodeIdFromQString(node_id);
  if (!IsColorGrade(id)) {
    SetLastError(tr("Only a Color Grade can be renamed"));
    return false;
  }
  const auto name = display_name.trimmed();
  if (name.isEmpty()) {
    SetLastError(tr("Color Grade name cannot be empty"));
    return false;
  }

  SetCommandActive(true);
  const auto reset_active = qScopeGuard([this] { SetCommandActive(false); });
  const auto result       = session_->SubmitRenameColorGrade(id, name.toStdString());
  if (EditorSessionResultIsFailure(result.kind)) {
    SetLastError(QString::fromStdString(result.message));
    return false;
  }
  refreshFromSession();
  selectNode(node_id);
  QueueProjectionApply();
  return true;
}

bool EditorNodeController::deleteColorGrade(const QString& node_id) {
  if (!ValidateCommandGeneration()) {
    return false;
  }
  const auto id    = NodeIdFromQString(node_id);
  const int  index = IndexOf(id);
  if (!IsColorGrade(id) || index < 0) {
    SetLastError(tr("Only a Color Grade can be deleted"));
    return false;
  }
  const NodeId successor = index + 1 < static_cast<int>(snapshot_.nodes.size())
                               ? snapshot_.nodes[static_cast<std::size_t>(index + 1)].node_id
                               : NodeId{};
  const NodeId predecessor =
      index > 0 ? snapshot_.nodes[static_cast<std::size_t>(index - 1)].node_id : NodeId{};

  SetCommandActive(true);
  const auto reset_active = qScopeGuard([this] { SetCommandActive(false); });
  const auto result       = session_->SubmitRemoveColorGrade(id);
  if (EditorSessionResultIsFailure(result.kind)) {
    SetLastError(QString::fromStdString(result.message));
    return false;
  }
  refreshFromSession();
  NodeId live_selection = successor;
  if (!ContainsNode(live_selection)) {
    live_selection = predecessor;
  }
  if (!ContainsNode(live_selection)) {
    live_selection = NodeId{"drt"};
  }
  if (ContainsNode(live_selection) && selected_node_id_ != live_selection) {
    selected_node_id_ = live_selection;
    SetLastError({});
    emit SelectionChanged();
    emit ActionAvailabilityChanged();
  }
  QueueProjectionApply();
  return true;
}

void EditorNodeController::SelectAt(int index) {
  if (!has_snapshot_ || index < 0 || index >= static_cast<int>(snapshot_.nodes.size())) {
    return;
  }
  selectNode(NodeIdToQString(snapshot_.nodes[static_cast<std::size_t>(index)].node_id));
}

void EditorNodeController::selectPreviousBackboneNode() {
  const int index = IndexOf(selected_node_id_);
  if (index < 0) {
    SelectAt(0);
    return;
  }
  SelectAt(std::max(0, index - 1));
}

void EditorNodeController::selectNextBackboneNode() {
  const int index = IndexOf(selected_node_id_);
  if (!has_snapshot_) {
    return;
  }
  const int last = static_cast<int>(snapshot_.nodes.size()) - 1;
  if (index < 0) {
    SelectAt(last);
    return;
  }
  SelectAt(std::min(last, index + 1));
}

void EditorNodeController::SelectByKind(EditorNodeKind kind) {
  if (!has_snapshot_) {
    return;
  }
  for (const auto& node : snapshot_.nodes) {
    if (node.node_kind == kind) {
      selectNode(NodeIdToQString(node.node_id));
      return;
    }
  }
}

void EditorNodeController::selectDevelop() { SelectByKind(EditorNodeKind::Develop); }

void EditorNodeController::selectDrt() { SelectByKind(EditorNodeKind::Drt); }

void RegisterEditorNodeQmlTypes() {
  qmlRegisterType<EditorNodeController>("Alcedo.Main", 1, 0, "EditorNodeController");
  qmlRegisterType<EditorNodeLayoutStore>("Alcedo.Main", 1, 0, "EditorNodeLayoutStore");
  qmlRegisterType<AlcedoQanGraph>("Alcedo.Main", 1, 0, "AlcedoQanGraph");
}

}  // namespace alcedo::ui
