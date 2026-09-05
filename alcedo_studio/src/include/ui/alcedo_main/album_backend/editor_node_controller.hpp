//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <memory>
#include <optional>

#include "app/editor_node_graph_draft.hpp"
#include "app/editor_node_graph_projection.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "ui/alcedo_main/album_backend/editor_node_layout_store.hpp"

namespace alcedo::ui {

class AlcedoQanGraph;
class EditorSessionController;

/**
 * @brief Application-layer owner of Nodes-page product selection and projection.
 *
 * Owns the session generation, selected NodeId, published snapshot, and the
 * incremental topology draft. It does not own Qan visuals or layout coordinates.
 * Add, Delete, and Connect mutate EditorNodeGraphDraft. The first admitted
 * operation that restores a complete supported graph submits one
 * NodeGraphTopologyChange. Visual connectors report exclusive-port requests.
 *
 * Threading: GUI thread only. Side effects: snapshot and selection signals.
 * Failure: stale generations and unknown NodeIds leave the live snapshot and
 * selection unchanged and set lastError.
 */
class EditorNodeController : public QObject {
  Q_OBJECT
  Q_PROPERTY(QObject* editorSession READ editor_session_object WRITE set_editor_session NOTIFY
                 EditorSessionChanged)
  Q_PROPERTY(
      QString selectedNodeId READ selected_node_id_string WRITE selectNode NOTIFY SelectionChanged)
  Q_PROPERTY(QStringList backboneNodeIds READ backbone_node_ids NOTIFY SnapshotChanged)
  Q_PROPERTY(quint64 projectionRevision READ projection_revision NOTIFY SnapshotChanged)
  Q_PROPERTY(quint64 topologyRevision READ topology_revision NOTIFY SnapshotChanged)
  Q_PROPERTY(quint64 elementId READ element_id NOTIFY SnapshotChanged)
  Q_PROPERTY(quint64 imageId READ image_id NOTIFY SnapshotChanged)
  Q_PROPERTY(QString versionId READ version_id NOTIFY SnapshotChanged)
  Q_PROPERTY(QString lastError READ last_error NOTIFY lastErrorChanged)
  Q_PROPERTY(bool hasSnapshot READ has_snapshot NOTIFY SnapshotChanged)
  Q_PROPERTY(bool commandActive READ command_active NOTIFY CommandStateChanged)
  Q_PROPERTY(bool canAddColorGrade READ can_add_color_grade NOTIFY ActionAvailabilityChanged)
  Q_PROPERTY(bool canRenameSelectedColorGrade READ can_rename_selected_color_grade NOTIFY
                 ActionAvailabilityChanged)
  Q_PROPERTY(bool canDeleteSelectedColorGrade READ can_delete_selected_color_grade NOTIFY
                 ActionAvailabilityChanged)
  Q_PROPERTY(bool canReconnectSelectedColorGrade READ can_reconnect_selected_color_grade NOTIFY
                 ActionAvailabilityChanged)
  Q_PROPERTY(bool incompleteDraft READ incomplete_draft NOTIFY DraftStateChanged)
  Q_PROPERTY(
      QString incompleteDraftInstruction READ incomplete_draft_instruction NOTIFY DraftStateChanged)
  Q_PROPERTY(QString selectedNodeName READ selected_node_name NOTIFY SelectionChanged)
  Q_PROPERTY(QObject* graphAdapter READ graph_adapter_object WRITE set_graph_adapter NOTIFY
                 GraphAdapterChanged)
  Q_PROPERTY(QObject* layoutStore READ layout_store_object WRITE set_layout_store NOTIFY
                 LayoutStoreChanged)

 public:
  explicit EditorNodeController(QObject* parent = nullptr);
  ~EditorNodeController() override;

  [[nodiscard]] auto editor_session_object() const -> QObject*;
  void               set_editor_session(QObject* session);

  [[nodiscard]] auto session() const -> EditorSessionController*;

  /**
   * @brief Replace the published snapshot after generation and revision checks.
   *
   * @param snapshot Immutable projection. Rejected when a session is bound and
   *        the generation does not match, or when a bound session already has a
   *        newer projection/topology revision.
   * @return false when the snapshot is rejected; lastError holds the reason.
   * @post On success, missing selection is restored to the first Color Grade or
   *       the first backbone node.
   */
  auto               PublishSnapshot(EditorNodeGraphSnapshot snapshot) -> bool;

  /**
   * @brief Build and publish a snapshot from @p document.
   *
   * @param document Live PipelineDocument under the read boundary.
   * @param session_generation Session value copied into the snapshot. Must match
   *        the bound session when one is set.
   * @return false when build or generation checks fail.
   */
  auto PublishDocument(const PipelineDocument& document, std::uint64_t session_generation) -> bool;

  /**
   * @brief Rebuild from the bound session's current document.
   *
   * No-op with a cleared snapshot when the session has no document. A build
   * error keeps the previous snapshot.
   */
  Q_INVOKABLE bool refreshFromSession();

  /**
   * @brief Project the active graph onto an AlcedoQanGraph adapter.
   *
   * Uses the committed snapshot, or materializes a draft snapshot at this
   * explicit projection boundary (page recreation). Does not apply selection;
   * ApplyBoundGraph applies layout and live selection after a successful
   * projection.
   *
   * @param adapter AlcedoQanGraph instance. Rejects null, wrong types, and a
   *        missing snapshot.
   * @return false when ApplySnapshot fails; lastError holds the adapter error.
   */
  Q_INVOKABLE bool applyToGraph(QObject* adapter);

  /**
   * @brief Select one product node.
   *
   * Unknown or empty ids fail closed and leave the current selection unchanged.
   * @param node_id Product NodeId string.
   */
  Q_INVOKABLE void selectNode(const QString& node_id);
  Q_INVOKABLE void selectPreviousBackboneNode();
  Q_INVOKABLE void selectNextBackboneNode();
  Q_INVOKABLE void selectDevelop();
  Q_INVOKABLE void selectDrt();
  /**
   * @brief Add one disconnected clean Color Grade below the main node DAG.
   *
   * Does not write PipelineDocument or history while the draft is incomplete.
   */
  Q_INVOKABLE bool addCleanColorGrade();
  /**
   * @brief Rename one Color Grade without changing its stable NodeId.
   * @return false for endpoints, blank names, stale generations, or history failure.
   */
  Q_INVOKABLE bool renameColorGrade(const QString& node_id, const QString& display_name);
  /**
   * @brief Remove one Color Grade from the draft. Does not bridge neighbors.
   */
  Q_INVOKABLE bool deleteColorGrade(const QString& node_id);
  /**
   * @brief Exclusive-port connect from @p source output to @p destination input.
   */
  Q_INVOKABLE bool requestConnect(const QString& source_node_id,
                                  const QString& destination_node_id);
  Q_INVOKABLE bool requestConnect(const QString& source_node_id, const QString& destination_node_id,
                                  quint64 request_generation);
  /**
   * @brief Resolve a visual-connector drop as exclusive-port Connect.
   *
   * Output-to-output drops are rejected. Any supported Color Grade or Develop
   * output may start a connection.
   */
  Q_INVOKABLE bool requestConnectorMove(const QString& source_node_id,
                                        const QString& destination_node_id,
                                        bool           destination_is_output);

  [[nodiscard]] auto selected_node_id() const -> NodeId { return selected_node_id_; }
  [[nodiscard]] auto selected_node_id_string() const -> QString;
  [[nodiscard]] auto backbone_node_ids() const -> QStringList;
  [[nodiscard]] auto session_generation() const -> quint64 { return session_generation_; }
  [[nodiscard]] auto projection_revision() const -> quint64 { return projection_revision_; }
  [[nodiscard]] auto topology_revision() const -> quint64 { return topology_revision_; }
  [[nodiscard]] auto element_id() const -> quint64 { return element_id_; }
  [[nodiscard]] auto image_id() const -> quint64 { return image_id_; }
  [[nodiscard]] auto version_id() const -> QString { return version_id_; }
  [[nodiscard]] auto last_error() const -> QString { return last_error_; }
  [[nodiscard]] auto has_snapshot() const -> bool { return has_snapshot_; }
  [[nodiscard]] auto command_active() const -> bool { return command_active_; }
  [[nodiscard]] auto can_add_color_grade() const -> bool;
  [[nodiscard]] auto can_rename_selected_color_grade() const -> bool;
  [[nodiscard]] auto can_delete_selected_color_grade() const -> bool;
  [[nodiscard]] auto can_reconnect_selected_color_grade() const -> bool;
  [[nodiscard]] auto incomplete_draft() const -> bool;
  [[nodiscard]] auto incomplete_draft_instruction() const -> QString;
  [[nodiscard]] auto selected_node_name() const -> QString;
  [[nodiscard]] auto snapshot() const -> const EditorNodeGraphSnapshot& { return snapshot_; }
  /**
   * @brief Nodes and edges currently shown on the page.
   *
   * Reads the incremental draft while it exists; otherwise the committed
   * snapshot. Does not copy the draft into snapshot_.
   */
  [[nodiscard]] auto ActiveNodes() const -> const std::vector<EditorNodeProjection>&;
  [[nodiscard]] auto ActiveEdges() const -> const std::vector<EditorNodeEdgeProjection>&;
  /// Queued ApplyBoundGraph requests, including coalesced repeats.
  [[nodiscard]] auto queued_projection_apply_count() const -> int {
    return queued_projection_apply_count_;
  }
  /// Successful adapter projection applies. Duplicate applies of the same
  /// committed revision are not counted.
  [[nodiscard]] auto completed_projection_apply_count() const -> int {
    return completed_projection_apply_count_;
  }
  /// Queued applies dropped because the adapter, generation, or attach identity was stale.
  [[nodiscard]] auto skipped_stale_projection_apply_count() const -> int {
    return skipped_stale_projection_apply_count_;
  }

  /**
   * @brief Bind the live Qan adapter owned by the open Nodes page.
   *
   * Null while the page is unloaded. When set and a snapshot exists, the
   * adapter is applied immediately so Add/Delete do not wait on QML Connections.
   */
  [[nodiscard]] auto graph_adapter_object() const -> QObject*;
  void               set_graph_adapter(QObject* adapter);
  /**
   * @brief Bind the layout store used to place nodes after a Qan apply.
   */
  [[nodiscard]] auto layout_store_object() const -> QObject*;
  void               set_layout_store(QObject* store);

  /**
   * @brief Set image/Version identity used by layout keys when no session is bound.
   */
  void               SetLayoutIdentity(quint64 element_id, quint64 image_id, QString version_id);

 signals:
  void EditorSessionChanged();
  void SnapshotChanged();
  void SelectionChanged();
  void lastErrorChanged();
  void CommandStateChanged();
  void ActionAvailabilityChanged();
  void GraphAdapterChanged();
  void LayoutStoreChanged();
  void DraftStateChanged();

 private:
  void               DisconnectSession();
  void               OnSessionStateChanged();
  void               OnSessionHistoryChanged();
  void               ClearSnapshot();
  void               SetLastError(QString error);
  void               RestoreSelectionAfterSnapshot();
  [[nodiscard]] auto ContainsNode(const NodeId& node_id) const -> bool;
  [[nodiscard]] auto DefaultSelectedNodeId() const -> NodeId;
  [[nodiscard]] auto IndexOf(const NodeId& node_id) const -> int;
  [[nodiscard]] auto NodeFor(const NodeId& node_id) const -> const EditorNodeProjection*;
  [[nodiscard]] auto ValidateCommandGeneration() -> bool;
  [[nodiscard]] auto IsColorGrade(const NodeId& node_id) const -> bool;
  void               SetCommandActive(bool active);
  void OnConnectorMoveRequested(const QString& source_node_id, const QString& destination_node_id,
                                bool destination_is_output);
  void OnConnectorRequestRejected(const QString& error);
  [[nodiscard]] auto CurrentDraftIdentity() const -> alcedo::EditorNodeGraphDraftIdentity;
  [[nodiscard]] auto EnsureDraft() -> bool;
  void               DiscardDraft();
  [[nodiscard]] auto ApplyDraftMutationToAdapter(
      const alcedo::EditorNodeGraphDraftMutation& mutation) -> bool;
  [[nodiscard]] auto MaybeSubmitDraft() -> bool;
  [[nodiscard]] auto TopologyChanged(const EditorNodeGraphSnapshot& snapshot) const -> bool;
  [[nodiscard]] auto BoundSessionGeneration() const -> std::optional<std::uint64_t>;
  void               SelectByKind(EditorNodeKind kind);
  void               SelectAt(int index);
  /**
   * @brief Apply the committed or draft projection, then layout and live selection.
   *
   * Activates the layout key before reading stored positions and drawers. QML
   * restores GraphView zoom and pan. Ordinary draft edits must not call this;
   * they use incremental adapter mutation.
   */
  void               ApplyBoundGraph();
  /// Apply the bound Qan adapter after the current GUI event so Add/Delete are
  /// not nested inside a GraphView key or menu handler.
  void               QueueProjectionApply();
  void               ApplyBoundGraphIfCurrent();
  void               SyncLayoutKey();
  void               PersistSavedSelection();
  void               ApplyLiveSelectionToAdapter();
  [[nodiscard]] auto SessionMatchesSubmittedIdentity() const -> bool;
  [[nodiscard]] auto SessionLocationChanged() const -> bool;
  [[nodiscard]] auto SessionIdentityChanged() const -> bool;
  [[nodiscard]] auto AdapterShowsCurrentCommittedProjection() const -> bool;
  void               AdoptCommittedDocument(const PipelineDocument& document);
  [[nodiscard]] auto HasActiveGraph() const -> bool;

  QPointer<EditorSessionController>                   session_;
  QPointer<AlcedoQanGraph>                            graph_adapter_;
  QPointer<EditorNodeLayoutStore>                     layout_store_;
  QMetaObject::Connection                             state_connection_;
  QMetaObject::Connection                             history_connection_;
  QMetaObject::Connection                             availability_connection_;
  QMetaObject::Connection                             graph_adapter_connection_;
  EditorNodeGraphSnapshot                             snapshot_{};
  bool                                                has_snapshot_ = false;
  NodeId                                              selected_node_id_;
  NodeId                                              selection_restore_node_id_;
  bool                                                command_active_          = false;
  bool                                                projection_apply_queued_ = false;
  quint64                                             session_generation_      = 0;
  quint64                                             observed_history_revision_ = 0;
  quint64                                             projection_revision_     = 0;
  quint64                                             topology_revision_       = 0;
  quint64                                             element_id_              = 0;
  quint64                                             image_id_                = 0;
  QString                                             version_id_;
  quint64                                             snapshot_element_id_ = 0;
  quint64                                             snapshot_image_id_   = 0;
  QString                                             snapshot_version_id_;
  QString                                             last_error_;
  std::unique_ptr<alcedo::EditorNodeGraphDraft>       draft_;
  std::optional<alcedo::EditorNodeGraphDraftIdentity> submitted_identity_;
  EditorNodeLayoutKey                                 last_layout_key_{};
  quint64                                             adapter_attach_generation_            = 0;
  quint64                                             pending_apply_attach_generation_      = 0;
  int                                                 queued_projection_apply_count_        = 0;
  int                                                 completed_projection_apply_count_     = 0;
  int                                                 skipped_stale_projection_apply_count_ = 0;
};

void RegisterEditorNodeQmlTypes();

}  // namespace alcedo::ui
