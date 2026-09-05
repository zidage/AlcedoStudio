//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_node_controller.hpp"

#include <qqml.h>

#include <QMetaObject>
#include <QScopeGuard>
#include <QUuid>
#include <algorithm>
#include <exception>
#include <utility>
#include <vector>

#include "ui/alcedo_main/album_backend/alcedo_qan_graph.hpp"
#include "ui/alcedo_main/album_backend/editor_node_graph_presentation.hpp"
#include "ui/alcedo_main/album_backend/editor_node_layout_store.hpp"
#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"
#include "ui/alcedo_main/shortcut_registry.hpp"

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

auto SameProjectionContent(const EditorNodeGraphSnapshot& lhs, const EditorNodeGraphSnapshot& rhs)
    -> bool {
  return lhs.nodes == rhs.nodes && lhs.edges == rhs.edges;
}

}  // namespace

EditorNodeController::EditorNodeController(QObject* parent) : QObject(parent) {}

EditorNodeController::~EditorNodeController() {
  if (graph_adapter_ != nullptr) {
    disconnect(graph_adapter_.data(), nullptr, this, nullptr);
  }
  graph_adapter_connection_ = {};
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
                                  &EditorNodeController::OnSessionStateChanged);
    history_connection_ = connect(session_.data(), &EditorSessionController::HistoryChanged, this,
                                  &EditorNodeController::OnSessionHistoryChanged);
    availability_connection_ =
        connect(session_.data(), &EditorSessionController::ActionAvailabilityChanged, this,
                &EditorNodeController::ActionAvailabilityChanged);
  }
  emit EditorSessionChanged();
  submitted_identity_.reset();
  DiscardDraft();
  if (session_ != nullptr) {
    if (SessionHidesGraph()) {
      ClearSnapshot();
    } else {
      refreshFromSession();
    }
  } else {
    ClearSnapshot();
  }
}

void EditorNodeController::SetLayoutIdentity(quint64 element_id, quint64 image_id,
                                             QString version_id) {
  const bool identity_changed =
      element_id_ != element_id || image_id_ != image_id || version_id_ != version_id;
  element_id_ = element_id;
  image_id_   = image_id;
  version_id_ = std::move(version_id);
  if (identity_changed) {
    SyncLayoutKey();
    if (has_snapshot_) {
      RestoreSelectionAfterSnapshot();
    }
  }
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
  DiscardDraft();
  snapshot_                  = {};
  has_snapshot_              = false;
  selected_node_id_          = {};
  selection_restore_node_id_ = {};
  session_generation_        = BoundSessionGeneration().value_or(0);
  projection_revision_       = 0;
  topology_revision_         = 0;
  snapshot_element_id_       = 0;
  snapshot_image_id_         = 0;
  snapshot_version_id_.clear();
  emit SnapshotChanged();
  emit SelectionChanged();
  emit ActionAvailabilityChanged();
}

auto EditorNodeController::HasActiveGraph() const -> bool {
  return draft_ != nullptr || has_snapshot_;
}

auto EditorNodeController::ActiveNodes() const -> const std::vector<EditorNodeProjection>& {
  if (draft_ != nullptr) {
    return draft_->Nodes();
  }
  return snapshot_.nodes;
}

auto EditorNodeController::ActiveEdges() const -> const std::vector<EditorNodeEdgeProjection>& {
  if (draft_ != nullptr) {
    return draft_->Edges();
  }
  return snapshot_.edges;
}

auto EditorNodeController::ContainsNode(const NodeId& node_id) const -> bool {
  return NodeFor(node_id) != nullptr;
}

auto EditorNodeController::DefaultSelectedNodeId() const -> NodeId {
  if (!HasActiveGraph()) {
    return {};
  }
  for (const auto& node : ActiveNodes()) {
    if (node.node_kind == EditorNodeKind::ColorGrade) {
      return node.node_id;
    }
  }
  for (const auto& node : ActiveNodes()) {
    if (node.node_kind == EditorNodeKind::Drt) {
      return node.node_id;
    }
  }
  return ActiveNodes().empty() ? NodeId{} : ActiveNodes().front().node_id;
}

auto EditorNodeController::IndexOf(const NodeId& node_id) const -> int {
  if (!HasActiveGraph()) {
    return -1;
  }
  const auto& nodes = ActiveNodes();
  for (int i = 0; i < static_cast<int>(nodes.size()); ++i) {
    if (nodes[static_cast<std::size_t>(i)].node_id == node_id) {
      return i;
    }
  }
  return -1;
}

auto EditorNodeController::NodeFor(const NodeId& node_id) const -> const EditorNodeProjection* {
  if (node_id.Empty() || !HasActiveGraph()) {
    return nullptr;
  }
  if (draft_ != nullptr) {
    return draft_->FindNode(node_id);
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
  if (layout_store_ != nullptr) {
    const auto key         = layout_store_->current_key();
    const bool key_changed = key != last_layout_key_;
    last_layout_key_       = key;
    if (key_changed) {
      const auto stored = layout_store_->selected_node_id();
      if (ContainsNode(stored)) {
        selected_node_id_          = stored;
        selection_restore_node_id_ = {};
        return;
      }
    }
  }
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
  if (!HasActiveGraph()) {
    return ids;
  }
  const auto& nodes = ActiveNodes();
  ids.reserve(static_cast<qsizetype>(nodes.size()));
  for (const auto& node : nodes) {
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
  return can_add_color_grade() && IsColorGrade(selected_node_id_) && draft_ == nullptr;
}

auto EditorNodeController::can_delete_selected_color_grade() const -> bool {
  return can_add_color_grade() && IsColorGrade(selected_node_id_);
}

auto EditorNodeController::incomplete_draft() const -> bool {
  return draft_ != nullptr && !draft_->SubmissionValid();
}

auto EditorNodeController::incomplete_draft_instruction() const -> QString {
  if (!incomplete_draft()) {
    return {};
  }
  return tr("Connect Color Grades into one Develop to DRT path.");
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
  if (has_snapshot_ && snapshot.session_generation == session_generation_ &&
      element_id_ == snapshot_element_id_ && image_id_ == snapshot_image_id_ &&
      version_id_ == snapshot_version_id_ && SameProjectionContent(snapshot, snapshot_)) {
    SetLastError({});
    return true;
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
  snapshot_element_id_         = element_id_;
  snapshot_image_id_           = image_id_;
  snapshot_version_id_         = version_id_;
  SyncLayoutKey();
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
  if (!HasActiveGraph()) {
    SetLastError(tr("The node graph has no snapshot"));
    return false;
  }
  EditorNodeGraphSnapshot        view;
  const EditorNodeGraphSnapshot* projected = &snapshot_;
  if (draft_ != nullptr) {
    view = draft_->CurrentSnapshot(session_generation_, projection_revision_, topology_revision_);
    projected = &view;
  } else if (!has_snapshot_) {
    SetLastError(tr("The node graph has no snapshot"));
    return false;
  }
  const auto promoted = graph->PromoteCommittedSnapshot(*projected);
  if (promoted.succeeded) {
    SetLastError({});
    return true;
  }
  const auto result = graph->ApplySnapshot(*projected);
  if (!result.succeeded) {
    SetLastError(result.error);
    return false;
  }
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
  if (graph_adapter_ != nullptr) {
    disconnect(graph_adapter_.data(), nullptr, this, nullptr);
  }
  graph_adapter_connection_ = {};
  graph_adapter_            = graph;
  ++adapter_attach_generation_;
  if (graph_adapter_ != nullptr) {
    graph_adapter_connection_ = connect(graph_adapter_.data(), &AlcedoQanGraph::GraphChanged, this,
                                        [this] { QueueProjectionApply(); });
    connect(graph_adapter_.data(), &AlcedoQanGraph::ConnectorMoveRequested, this,
            &EditorNodeController::OnConnectorMoveRequested);
    connect(graph_adapter_.data(), &AlcedoQanGraph::ConnectorRequestRejected, this,
            &EditorNodeController::OnConnectorRequestRejected);
  }
  emit GraphAdapterChanged();
  if (graph_adapter_ != nullptr && HasActiveGraph() && graph_adapter_->graph() != nullptr) {
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
  SyncLayoutKey();
}

void EditorNodeController::SyncLayoutKey() {
  if (layout_store_ == nullptr) {
    return;
  }
  layout_store_->activate(QString(), element_id_, image_id_, version_id_);
}

void EditorNodeController::PersistSavedSelection() {
  if (layout_store_ == nullptr) {
    return;
  }
  layout_store_->set_selected_node_id(selected_node_id_);
}

void EditorNodeController::ApplyLiveSelectionToAdapter() {
  if (graph_adapter_ == nullptr) {
    return;
  }
  graph_adapter_->ApplyProductSelection(selected_node_id_);
}

auto EditorNodeController::AdapterShowsCurrentCommittedProjection() const -> bool {
  if (draft_ != nullptr || graph_adapter_.isNull() || !graph_adapter_->has_projection()) {
    return false;
  }
  return graph_adapter_->session_generation() == session_generation_ &&
         graph_adapter_->topology_revision() == topology_revision_ &&
         graph_adapter_->projection_revision() == projection_revision_;
}

void EditorNodeController::ApplyBoundGraph() {
  if (graph_adapter_.isNull() || graph_adapter_->graph() == nullptr || !HasActiveGraph()) {
    return;
  }
  const auto bound = BoundSessionGeneration();
  if (bound.has_value() && session_generation_ != *bound) {
    ++skipped_stale_projection_apply_count_;
    return;
  }
  SyncLayoutKey();
  if (AdapterShowsCurrentCommittedProjection()) {
    ApplyLiveSelectionToAdapter();
    return;
  }
  if (!applyToGraph(graph_adapter_.data())) {
    return;
  }
  ++completed_projection_apply_count_;
  auto* layout = layout_store_.data();
  if (layout != nullptr) {
    layout->EnsureDefaultPositions(draft_ == nullptr ? snapshot_
                                                     : draft_->CurrentSnapshot(session_generation_,
                                                                               projection_revision_,
                                                                               topology_revision_));
    for (const auto& node : ActiveNodes()) {
      const auto id = NodeIdToQString(node.node_id);
      if (layout->hasNodePosition(id)) {
        const auto pos = layout->nodePosition(id);
        graph_adapter_->setNodePosition(id, pos.x(), pos.y());
      }
      graph_adapter_->setDrawerOpen(id, layout->drawerOpen(id));
    }
  }
  ApplyLiveSelectionToAdapter();
}

void EditorNodeController::QueueProjectionApply() {
  ++queued_projection_apply_count_;
  pending_apply_attach_generation_ = adapter_attach_generation_;
  if (projection_apply_queued_) {
    return;
  }
  projection_apply_queued_ = true;
  QMetaObject::invokeMethod(this, [this] { ApplyBoundGraphIfCurrent(); }, Qt::QueuedConnection);
}

void EditorNodeController::ApplyBoundGraphIfCurrent() {
  projection_apply_queued_ = false;
  if (graph_adapter_.isNull() || graph_adapter_->graph() == nullptr) {
    ++skipped_stale_projection_apply_count_;
    return;
  }
  if (adapter_attach_generation_ != pending_apply_attach_generation_) {
    ++skipped_stale_projection_apply_count_;
    return;
  }
  ApplyBoundGraph();
}

bool EditorNodeController::refreshFromSession() {
  if (session_ == nullptr) {
    ClearSnapshot();
    SetLastError(tr("No editor session is bound"));
    return false;
  }
  element_id_                = session_->element_id();
  image_id_                  = session_->image_id();
  version_id_                = session_->active_version_id();
  observed_history_revision_ = session_->history_revision();
  SyncLayoutKey();
  const auto* document = session_->pipeline_document();
  if (document == nullptr) {
    ClearSnapshot();
    SetLastError({});
    emit SnapshotChanged();
    return false;
  }
  return PublishDocument(*document, static_cast<std::uint64_t>(session_->session_generation()));
}

auto EditorNodeController::SessionMatchesSubmittedIdentity() const -> bool {
  if (!submitted_identity_.has_value() || session_ == nullptr) {
    return false;
  }
  const auto& submitted = *submitted_identity_;
  return submitted.element_id == static_cast<std::uint64_t>(session_->element_id()) &&
         submitted.image_id == static_cast<std::uint64_t>(session_->image_id()) &&
         QString::fromStdString(submitted.version_id) == session_->active_version_id() &&
         submitted.session_generation == static_cast<std::uint64_t>(session_->session_generation());
}

[[nodiscard]] auto EditorNodeController::SessionIdentityChanged() const -> bool {
  return SessionLocationChanged() ||
         (session_ != nullptr && session_->active_version_id() != version_id_);
}

[[nodiscard]] auto EditorNodeController::SessionLocationChanged() const -> bool {
  if (session_ == nullptr) {
    return false;
  }
  return static_cast<quint64>(session_->element_id()) != element_id_ ||
         static_cast<quint64>(session_->image_id()) != image_id_ ||
         static_cast<quint64>(session_->session_generation()) != session_generation_;
}

auto EditorNodeController::SessionHidesGraph() const -> bool {
  if (session_ == nullptr) {
    return true;
  }
  switch (session_->session_state()) {
    case alcedo::EditorSessionState::Interactive:
    case alcedo::EditorSessionState::Saving:
      return false;
    case alcedo::EditorSessionState::NoImage:
    case alcedo::EditorSessionState::Acquiring:
    case alcedo::EditorSessionState::Loading:
    case alcedo::EditorSessionState::Switching:
    case alcedo::EditorSessionState::RetainedImageFailure:
    case alcedo::EditorSessionState::Failed:
    case alcedo::EditorSessionState::ShuttingDown:
      return true;
  }
  return true;
}

void EditorNodeController::OnSessionStateChanged() {
  if (session_ == nullptr) {
    submitted_identity_.reset();
    DiscardDraft();
    ClearSnapshot();
    return;
  }
  if (SessionHidesGraph()) {
    submitted_identity_.reset();
    DiscardDraft();
    ClearSnapshot();
    return;
  }
  if (SessionLocationChanged() || !has_snapshot_) {
    submitted_identity_.reset();
    DiscardDraft();
    refreshFromSession();
  }
}

void EditorNodeController::OnSessionHistoryChanged() {
  if (session_ == nullptr || SessionHidesGraph()) {
    return;
  }
  const auto history_revision = session_->history_revision();
  if (history_revision == observed_history_revision_) {
    return;
  }
  observed_history_revision_ = history_revision;
  if (SessionIdentityChanged()) {
    submitted_identity_.reset();
    DiscardDraft();
    refreshFromSession();
    return;
  }
  if (submitted_identity_.has_value() && SessionMatchesSubmittedIdentity()) {
    return;
  }
  submitted_identity_.reset();
  if (session_ != nullptr && draft_ != nullptr) {
    const auto* document = session_->pipeline_document();
    if (document != nullptr && draft_->MatchesIdentity(CurrentDraftIdentity()) &&
        draft_->MatchesBase(*document)) {
      return;
    }
    DiscardDraft();
  }
  refreshFromSession();
}

void EditorNodeController::selectNode(const QString& node_id) {
  const auto id = NodeIdFromQString(node_id);
  if (!ContainsNode(id)) {
    SetLastError(tr("That node is not in the current graph"));
    return;
  }
  if (selected_node_id_ == id) {
    selection_restore_node_id_ = {};
    SetLastError({});
    PersistSavedSelection();
    ApplyLiveSelectionToAdapter();
    return;
  }
  selected_node_id_          = id;
  selection_restore_node_id_ = {};
  SetLastError({});
  PersistSavedSelection();
  ApplyLiveSelectionToAdapter();
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
  if (!EnsureDraft()) {
    return false;
  }
  const auto   uuid = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower().toStdString();
  const NodeId new_id{"grade." + uuid};
  SetCommandActive(true);
  const auto reset_active = qScopeGuard([this] { SetCommandActive(false); });
  auto       mutation     = draft_->AddColorGrade(new_id);
  if (!mutation.succeeded) {
    SetLastError(PresentNodeGraphDraftMutation(mutation));
    return false;
  }
  if (!ApplyDraftMutationToAdapter(mutation)) {
    return false;
  }
  if (layout_store_ != nullptr) {
    layout_store_->AssignStagingPosition(new_id, snapshot_);
    const auto pos = layout_store_->NodePosition(new_id);
    if (graph_adapter_ != nullptr && pos.has_value()) {
      graph_adapter_->SetNodeItemPosition(new_id, *pos);
    }
  }
  emit DraftStateChanged();
  emit ActionAvailabilityChanged();
  selectNode(NodeIdToQString(new_id));
  return MaybeSubmitDraft();
}

bool EditorNodeController::renameColorGrade(const QString& node_id, const QString& display_name) {
  if (!ValidateCommandGeneration()) {
    return false;
  }
  if (draft_ != nullptr) {
    SetLastError(tr("Finish the node graph before renaming"));
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
  if (alcedo::EditorSessionResultIsFailure(result.kind)) {
    SetLastError(QString::fromStdString(result.message));
    return false;
  }
  refreshFromSession();
  selectNode(node_id);
  return true;
}

bool EditorNodeController::deleteColorGrade(const QString& node_id) {
  if (!ValidateCommandGeneration()) {
    return false;
  }
  const auto id = NodeIdFromQString(node_id);
  if (!IsColorGrade(id)) {
    SetLastError(tr("Only a Color Grade can be deleted"));
    return false;
  }
  if (!EnsureDraft()) {
    return false;
  }
  SetCommandActive(true);
  const auto reset_active = qScopeGuard([this] { SetCommandActive(false); });
  auto       mutation     = draft_->RemoveColorGrade(id);
  if (!mutation.succeeded) {
    SetLastError(PresentNodeGraphDraftMutation(mutation));
    return false;
  }
  if (!ApplyDraftMutationToAdapter(mutation)) {
    return false;
  }
  emit DraftStateChanged();
  emit ActionAvailabilityChanged();
  if (!ContainsNode(selected_node_id_)) {
    selected_node_id_ = DefaultSelectedNodeId();
    PersistSavedSelection();
    ApplyLiveSelectionToAdapter();
    emit SelectionChanged();
    emit ActionAvailabilityChanged();
  }
  return MaybeSubmitDraft();
}

bool EditorNodeController::requestConnect(const QString& source_node_id,
                                          const QString& destination_node_id) {
  return requestConnect(source_node_id, destination_node_id, session_generation_);
}

bool EditorNodeController::requestConnect(const QString& source_node_id,
                                          const QString& destination_node_id,
                                          quint64        request_generation) {
  if (!ValidateCommandGeneration()) {
    if (graph_adapter_ != nullptr) {
      graph_adapter_->hideConnectorPreview();
    }
    return false;
  }
  if (request_generation != session_generation_) {
    SetLastError(tr("The node command is from another editor session"));
    if (graph_adapter_ != nullptr) {
      graph_adapter_->hideConnectorPreview();
    }
    return false;
  }
  if (!EnsureDraft()) {
    if (graph_adapter_ != nullptr) {
      graph_adapter_->hideConnectorPreview();
    }
    return false;
  }
  SetCommandActive(true);
  const auto reset_active = qScopeGuard([this] { SetCommandActive(false); });
  auto       mutation =
      draft_->Connect(NodeIdFromQString(source_node_id), NodeIdFromQString(destination_node_id));
  if (graph_adapter_ != nullptr) {
    graph_adapter_->hideConnectorPreview();
  }
  if (!mutation.succeeded) {
    SetLastError(PresentNodeGraphDraftMutation(mutation));
    return false;
  }
  if (mutation.no_op) {
    SetLastError({});
    return true;
  }
  if (!ApplyDraftMutationToAdapter(mutation)) {
    return false;
  }
  emit DraftStateChanged();
  emit ActionAvailabilityChanged();
  return MaybeSubmitDraft();
}

bool EditorNodeController::requestConnectorMove(const QString& source_node_id,
                                                const QString& destination_node_id,
                                                bool           destination_is_output) {
  if (destination_is_output) {
    SetLastError(tr("Connections must go from an output port to an input port"));
    if (graph_adapter_ != nullptr) {
      graph_adapter_->hideConnectorPreview();
    }
    return false;
  }
  return requestConnect(source_node_id, destination_node_id, session_generation_);
}

void EditorNodeController::OnConnectorMoveRequested(const QString& source_node_id,
                                                    const QString& destination_node_id,
                                                    bool           destination_is_output) {
  requestConnectorMove(source_node_id, destination_node_id, destination_is_output);
}

void EditorNodeController::OnConnectorRequestRejected(const QString& error) {
  if (graph_adapter_ != nullptr) {
    graph_adapter_->hideConnectorPreview();
  }
  SetLastError(error);
}

auto EditorNodeController::CurrentDraftIdentity() const -> alcedo::EditorNodeGraphDraftIdentity {
  alcedo::EditorNodeGraphDraftIdentity identity;
  identity.element_id          = element_id_;
  identity.image_id            = image_id_;
  identity.version_id          = version_id_.toStdString();
  identity.session_generation  = session_generation_;
  identity.projection_revision = projection_revision_;
  identity.topology_revision   = topology_revision_;
  return identity;
}

auto EditorNodeController::EnsureDraft() -> bool {
  if (draft_ != nullptr) {
    if (!draft_->MatchesIdentity(CurrentDraftIdentity())) {
      SetLastError(tr("The node command is from another editor session"));
      return false;
    }
    return true;
  }
  if (session_ == nullptr) {
    SetLastError(tr("No editor session is bound"));
    return false;
  }
  const auto* document = session_->pipeline_document();
  if (document == nullptr) {
    SetLastError(tr("No editable node graph is available"));
    return false;
  }
  try {
    draft_ = std::make_unique<alcedo::EditorNodeGraphDraft>(
        alcedo::EditorNodeGraphDraft::FromDocument(*document, CurrentDraftIdentity()));
  } catch (const std::exception& ex) {
    SetLastError(QString::fromUtf8(ex.what()));
    return false;
  }
  emit DraftStateChanged();
  return true;
}

void EditorNodeController::DiscardDraft() {
  if (draft_ == nullptr) {
    return;
  }
  draft_.reset();
  emit DraftStateChanged();
}

void EditorNodeController::AdoptCommittedDocument(const PipelineDocument& document) {
  auto built                = EditorNodeGraphProjection::Build(document, session_generation_, 0, 0);
  topology_revision_        = topology_revision_ + 1;
  projection_revision_      = projection_revision_ + 1;
  built.session_generation  = session_generation_;
  built.topology_revision   = topology_revision_;
  built.projection_revision = projection_revision_;
  snapshot_                 = std::move(built);
  has_snapshot_             = true;
}

auto EditorNodeController::ApplyDraftMutationToAdapter(
    const alcedo::EditorNodeGraphDraftMutation& mutation) -> bool {
  if (graph_adapter_ == nullptr || graph_adapter_->graph() == nullptr) {
    return true;
  }
  const auto result = graph_adapter_->ApplyMutation(mutation);
  if (result.succeeded) {
    return true;
  }
  if (draft_ != nullptr) {
    draft_->RestoreLastMutation();
  }
  SetLastError(result.error);
  return false;
}

auto EditorNodeController::MaybeSubmitDraft() -> bool {
  if (draft_ == nullptr) {
    return true;
  }
  if (!draft_->SubmissionValid()) {
    SetLastError({});
    emit DraftStateChanged();
    emit ActionAvailabilityChanged();
    return true;
  }
  if (draft_->DeltaEmpty()) {
    DiscardDraft();
    SetLastError({});
    emit ActionAvailabilityChanged();
    return true;
  }
  auto change         = draft_->MakeChange();
  submitted_identity_ = CurrentDraftIdentity();
  const auto result   = session_->SubmitNodeGraphTopologyEdit(change);
  if (alcedo::EditorSessionResultIsFailure(result.kind)) {
    submitted_identity_.reset();
    SetLastError(QString::fromStdString(result.message));
    emit DraftStateChanged();
    return false;
  }
  DiscardDraft();
  QString projection_error;
  if (session_ != nullptr && session_->pipeline_document() != nullptr) {
    try {
      AdoptCommittedDocument(*session_->pipeline_document());
    } catch (const std::exception& ex) {
      projection_error = QString::fromUtf8(ex.what());
      SetLastError(projection_error);
    }
  }
  if (graph_adapter_ != nullptr) {
    const auto promoted = graph_adapter_->PromoteCommittedSnapshot(snapshot_);
    if (!promoted.succeeded) {
      if (!promoted.error.isEmpty()) {
        projection_error = promoted.error;
        SetLastError(projection_error);
      }
      QueueProjectionApply();
    } else {
      ApplyLiveSelectionToAdapter();
    }
  }
  submitted_identity_.reset();
  if (projection_error.isEmpty()) {
    SetLastError({});
  }
  emit SnapshotChanged();
  emit ActionAvailabilityChanged();
  return true;
}

void EditorNodeController::SelectAt(int index) {
  if (!HasActiveGraph() || index < 0 || index >= static_cast<int>(ActiveNodes().size())) {
    return;
  }
  selectNode(NodeIdToQString(ActiveNodes()[static_cast<std::size_t>(index)].node_id));
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
  if (!HasActiveGraph()) {
    return;
  }
  const int last = static_cast<int>(ActiveNodes().size()) - 1;
  if (index < 0) {
    SelectAt(last);
    return;
  }
  SelectAt(std::min(last, index + 1));
}

void EditorNodeController::SelectByKind(EditorNodeKind kind) {
  if (!HasActiveGraph()) {
    return;
  }
  for (const auto& node : ActiveNodes()) {
    if (node.node_kind == kind) {
      selectNode(NodeIdToQString(node.node_id));
      return;
    }
  }
}

void EditorNodeController::selectDevelop() { SelectByKind(EditorNodeKind::Develop); }

void EditorNodeController::selectDrt() { SelectByKind(EditorNodeKind::Drt); }

void RegisterEditorNodeQmlTypes() {
  RegisterShortcutRegistryQmlType();
  qmlRegisterType<EditorNodeController>("Alcedo.Main", 1, 0, "EditorNodeController");
  qmlRegisterType<EditorNodeLayoutStore>("Alcedo.Main", 1, 0, "EditorNodeLayoutStore");
  qmlRegisterType<AlcedoQanGraph>("Alcedo.Main", 1, 0, "AlcedoQanGraph");
}

}  // namespace alcedo::ui
