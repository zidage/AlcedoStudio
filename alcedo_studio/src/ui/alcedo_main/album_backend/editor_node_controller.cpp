//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_node_controller.hpp"

#include <qqml.h>

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

EditorNodeController::~EditorNodeController() { DisconnectSession(); }

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
  snapshot_            = {};
  has_snapshot_        = false;
  selected_node_id_    = {};
  session_generation_  = BoundSessionGeneration().value_or(0);
  projection_revision_ = 0;
  topology_revision_   = 0;
  emit SnapshotChanged();
  emit SelectionChanged();
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

auto EditorNodeController::TopologyChanged(const EditorNodeGraphSnapshot& snapshot) const -> bool {
  if (!has_snapshot_) {
    return true;
  }
  return !SameTopology(snapshot_, snapshot);
}

void EditorNodeController::RestoreSelectionAfterSnapshot() {
  if (ContainsNode(selected_node_id_)) {
    return;
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
    session_generation_  = snapshot.session_generation;
    topology_revision_   = snapshot.topology_revision == 0 ? 1 : snapshot.topology_revision;
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
    SetLastError({});
    return;
  }
  selected_node_id_ = id;
  SetLastError({});
  emit SelectionChanged();
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
