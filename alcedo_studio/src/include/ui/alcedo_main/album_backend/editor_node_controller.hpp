//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <cstdint>
#include <memory>
#include <vector>

#include "app/editor_node_graph_draft.hpp"
#include "app/editor_node_graph_projection.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "ui/alcedo_main/album_backend/editor_node_layout_store.hpp"
#include "ui/alcedo_main/album_backend/mask_thumbnail_coordinator.hpp"

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
 * Failure: unavailable sessions and unknown NodeIds leave the live snapshot
 * and selection unchanged and set lastError.
 */
class EditorNodeController : public QObject {
  Q_OBJECT
  Q_PROPERTY(QObject* editorSession READ editor_session_object WRITE set_editor_session NOTIFY
                 EditorSessionChanged)
  Q_PROPERTY(
      QString selectedNodeId READ selected_node_id_string WRITE selectNode NOTIFY SelectionChanged)
  /// All selected product NodeIds in recency order; the last entry is the
  /// primary node that drives single-node actions.
  Q_PROPERTY(QStringList selectedNodeIds READ selected_node_ids NOTIFY SelectionChanged)
  Q_PROPERTY(int selectedNodeCount READ selected_node_count NOTIFY SelectionChanged)
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
  /// True when every selected node is a deletable Color Grade and the graph is
  /// editable. Drives the multi-selection Delete action.
  Q_PROPERTY(bool canDeleteSelectedNodes READ can_delete_selected_nodes NOTIFY
                 ActionAvailabilityChanged)
  Q_PROPERTY(bool incompleteDraft READ incomplete_draft NOTIFY DraftStateChanged)
  Q_PROPERTY(
      QString incompleteDraftInstruction READ incomplete_draft_instruction NOTIFY DraftStateChanged)
  /// NodeIds outside the draft's Develop-to-DRT path; empty without a draft.
  Q_PROPERTY(QStringList detachedDraftNodeIds READ detached_draft_node_ids NOTIFY DraftStateChanged)
  /// Committed Mask Groups rows in backbone execution order. The panel is a
  /// read-only projection of the same document as the node graph.
  Q_PROPERTY(QVariantList maskGroups READ mask_groups NOTIFY MaskGroupsChanged)
  /// False while a node-graph draft exists; structural group commands and Mask
  /// creation stay disabled until the draft is committed or reverted.
  Q_PROPERTY(bool canEditMaskGroupStructure READ can_edit_mask_group_structure NOTIFY
                 ActionAvailabilityChanged)
  Q_PROPERTY(QString selectedNodeName READ selected_node_name NOTIFY SelectionChanged)
  Q_PROPERTY(QString selectedNodeKind READ selected_node_kind NOTIFY SelectionChanged)
  Q_PROPERTY(QStringList supportedAdjustmentPanels READ supported_adjustment_panels NOTIFY
                 SelectionChanged)
  Q_PROPERTY(QVariantList selectedNodeMasks READ selected_node_masks NOTIFY SelectionChanged)
  Q_PROPERTY(QObject* graphAdapter READ graph_adapter_object WRITE set_graph_adapter NOTIFY
                 GraphAdapterChanged)
  Q_PROPERTY(QObject* layoutStore READ layout_store_object WRITE set_layout_store NOTIFY
                 LayoutStoreChanged)
  Q_PROPERTY(QObject* maskThumbnails READ mask_thumbnails_object CONSTANT)

 public:
  explicit EditorNodeController(QObject* parent = nullptr);
  ~EditorNodeController() override;

  [[nodiscard]] auto editor_session_object() const -> QObject*;
  void               set_editor_session(QObject* session);

  [[nodiscard]] auto session() const -> EditorSessionController*;

  /**
   * @brief Replace the published snapshot.
   *
   * @param snapshot Immutable projection. The caller stamps the session value
   *        that identifies the producing image-load session.
   * @return false when the snapshot has no nodes; lastError holds the reason.
   * @post On success, missing selection is restored to the first Color Grade or
   *       the first backbone node.
   */
  auto               PublishSnapshot(EditorNodeGraphSnapshot snapshot) -> bool;

  /**
   * @brief Build and publish a snapshot from @p document.
   *
   * @param document Live PipelineDocument under the read boundary.
   * @param session_generation Session value stamped onto the snapshot.
   * @return false when the projection build fails or the snapshot has no nodes.
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
  /**
   * @brief Toggle @p node_id in the controller-owned selection.
   *
   * An unselected node joins the selection and becomes the primary node. An
   * already-selected node leaves; when it was primary the most recently
   * selected remaining node becomes primary. Unknown or empty ids fail closed.
   */
  Q_INVOKABLE void toggleNodeSelection(const QString& node_id);
  /**
   * @brief Extend the selection one backbone step without collapsing it.
   *
   * Moves the primary node one position (@p direction < 0 toward Develop,
   * otherwise toward DRT/Post) and keeps every currently selected node
   * selected. With no selection it behaves like the plain arrow commands.
   */
  Q_INVOKABLE void extendNodeSelectionByStep(int direction);
  /// Clear the controller-owned selection.
  Q_INVOKABLE void clearNodeSelection();
  /// True when @p node_id is part of the current selection.
  Q_INVOKABLE bool isNodeSelected(const QString& node_id) const;
  /// Select the panel owner, returning to the last live Color Grade when possible.
  void             SelectNodeForAdjustmentPanel(const QString& panel);
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
   * @brief Mask Groups: insert one clean Color Grade at the top of the stack.
   *
   * Committed-document operation: the new node becomes the final Color Grade
   * before DRT/Post through one typed history commit. Rejects while a
   * node-graph draft exists; the draft must be completed or reverted first.
   * On success the new group is selected.
   */
  Q_INVOKABLE bool insertMaskGroupAtTop();
  /**
   * @brief Mask Groups: remove one Color Grade and bridge its neighbors.
   *
   * Committed-document operation through one typed history commit. Rejects
   * endpoints, non-Color-Grade ids, and any live draft.
   */
  Q_INVOKABLE bool removeMaskGroup(const QString& node_id);
  /**
   * @brief Mask Groups: move one Color Grade to @p target_index in the
   * downstream-first group order the panel displays (0 = nearest DRT/Post).
   *
   * Converts the reordered backbone into the same NodeGraphTopologyChange used
   * by the Nodes page, then submits one history commit and one topology render
   * request. Out-of-range indices clamp to the list ends; targeting the current
   * index is an accepted no-op that submits nothing. Rejects endpoints,
   * non-Color-Grade ids, and any live draft.
   */
  Q_INVOKABLE bool moveMaskGroupToIndex(const QString& node_id, int target_index);
  /**
   * @brief Switch the tool panel to the Nodes page and select @p node_id.
   *
   * The incomplete-draft boundary action: the Mask Groups panel points at a
   * detached draft node so the user can finish wiring it there.
   */
  Q_INVOKABLE bool locateNodeInGraph(const QString& node_id);
  /**
   * @brief Rename one Color Grade without changing its stable NodeId.
   * @return false for endpoints, blank names, or history failure.
   */
  Q_INVOKABLE bool renameColorGrade(const QString& node_id, const QString& display_name);
  /**
   * @brief Change deletion-only protection through session history without rendering.
   * @return false for endpoints, unfinished drafts, or history failure.
   */
  Q_INVOKABLE bool setColorGradeDeletionProtected(const QString& node_id, bool deletion_protected);
  /**
   * @brief Remove one Color Grade from the draft. Does not bridge neighbors.
   */
  Q_INVOKABLE bool deleteColorGrade(const QString& node_id);
  /**
   * @brief Remove every selected Color Grade through one draft mutation.
   *
   * Validates the whole selection first: any non-Color-Grade, protected, or
   * unknown member rejects the request and leaves the selection, the draft,
   * Qan visuals, and history untouched. On success all selected nodes and
   * their incident edges leave the draft together; neighbors are not bridged.
   * Selection falls to the surviving node at the first removed position.
   */
  Q_INVOKABLE bool deleteSelectedNodes();
  /**
   * @brief Exclusive-port connect from @p source output to @p destination input.
   */
  Q_INVOKABLE bool requestConnect(const QString& source_node_id,
                                  const QString& destination_node_id);
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
  /// Selected ids in recency order; the last entry equals selected_node_id().
  [[nodiscard]] auto selected_node_ids() const -> QStringList;
  [[nodiscard]] auto selected_node_count() const -> int {
    return static_cast<int>(selected_node_ids_.size());
  }
  [[nodiscard]] auto backbone_node_ids() const -> QStringList;
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
  /// True when the selection is non-empty and every member is a Color Grade.
  [[nodiscard]] auto can_delete_selected_nodes() const -> bool;
  [[nodiscard]] auto incomplete_draft() const -> bool;
  [[nodiscard]] auto incomplete_draft_instruction() const -> QString;
  /// True while an uncommitted node-graph draft exists (incomplete or failed submit).
  [[nodiscard]] auto has_draft() const -> bool { return draft_ != nullptr; }
  /// Draft nodes outside the Develop-to-DRT path; empty without a draft.
  [[nodiscard]] auto detached_draft_node_ids() const -> QStringList;
  /// Committed Mask Groups rows as QVariant maps for the panel. Empty without
  /// a snapshot. Read-only; structural edits go through insertMaskGroupAtTop /
  /// removeMaskGroup.
  [[nodiscard]] auto mask_groups() const -> QVariantList;
  /// Latest committed Mask Groups snapshot for tests and same-thread readers.
  [[nodiscard]] auto mask_group_snapshot() const -> const alcedo::EditorMaskGroupSnapshot& {
    return mask_group_snapshot_;
  }
  [[nodiscard]] auto has_mask_group_snapshot() const -> bool { return has_mask_group_snapshot_; }
  /// Structural Mask Groups commands require a committed graph with no draft.
  [[nodiscard]] auto can_edit_mask_group_structure() const -> bool;
  [[nodiscard]] auto selected_node_name() const -> QString;
  /// Product kind key: develop, colorGrade, or drt. Empty when nothing is selected.
  [[nodiscard]] auto selected_node_kind() const -> QString;
  /// Panel keys the selected node may show. Empty when nothing is selected.
  [[nodiscard]] auto supported_adjustment_panels() const -> QStringList;
  /// Read-only Mask identity rows for the selected Color Grade. Empty otherwise.
  [[nodiscard]] auto selected_node_masks() const -> QVariantList;
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
  /// Successful adapter projection applies.
  [[nodiscard]] auto completed_projection_apply_count() const -> int {
    return completed_projection_apply_count_;
  }
  /// Queued applies dropped because no adapter was bound or the bound adapter
  /// changed before the queued apply ran.
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
  [[nodiscard]] auto mask_thumbnails_object() const -> QObject*;

  /**
   * @brief Set image/Version identity used by layout keys when no session is bound.
   */
  void               SetLayoutIdentity(quint64 element_id, quint64 image_id, QString version_id);

 signals:
  void EditorSessionChanged();
  void SnapshotChanged();
  void snapshotChanged();
  void SelectionChanged();
  void selectionChanged();
  void lastErrorChanged();
  void CommandStateChanged();
  void ActionAvailabilityChanged();
  void GraphAdapterChanged();
  void LayoutStoreChanged();
  void DraftStateChanged();
  void MaskGroupsChanged();

 private:
  void               DisconnectSession();
  void               OnSessionStateChanged();
  void               OnSessionHistoryChanged();
  void               ClearSnapshot();
  void               SetLastError(QString error);
  /// @p select_default_color_grade is true only for a new image-load generation.
  /// Topology edits that drop the current node leave selection empty.
  void               RestoreSelectionAfterSnapshot(bool select_default_color_grade);
  void               SyncSessionAdjustmentNode(bool seal_open_sequence);
  [[nodiscard]] auto ContainsNode(const NodeId& node_id) const -> bool;
  /// Product default Color Grade (`grade.primary`) when that node exists.
  [[nodiscard]] auto DefaultSelectedNodeId() const -> NodeId;
  [[nodiscard]] auto IndexOf(const NodeId& node_id) const -> int;
  [[nodiscard]] auto NodeFor(const NodeId& node_id) const -> const EditorNodeProjection*;
  /// Rejects the command and sets lastError when a command is already active,
  /// no editable graph is bound, or the session is not editable.
  [[nodiscard]] auto ValidateCommandState() -> bool;
  [[nodiscard]] auto IsColorGrade(const NodeId& node_id) const -> bool;
  void               SetCommandActive(bool active);
  void OnConnectorMoveRequested(const QString& source_node_id, const QString& destination_node_id,
                                bool destination_is_output);
  void OnConnectorRequestRejected(const QString& error);
  [[nodiscard]] auto EnsureDraft() -> bool;
  void               DiscardDraft();
  [[nodiscard]] auto ApplyDraftMutationToAdapter(
      const alcedo::EditorNodeGraphDraftMutation& mutation) -> bool;
  [[nodiscard]] auto MaybeSubmitDraft() -> bool;
  /// Publish the committed Mask Groups projection beside the node snapshot.
  /// Equal content republishes nothing.
  [[nodiscard]] auto PublishMaskGroupSnapshot(alcedo::EditorMaskGroupSnapshot snapshot) -> bool;
  [[nodiscard]] auto TopologyChanged(const EditorNodeGraphSnapshot& snapshot) const -> bool;
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
  /// Fill missing backbone positions, push nodes below grown predecessors, then
  /// push stored positions and drawer state to the adapter. Runs after a
  /// projection apply and on every layout-store change (drawer folds, drags) so
  /// a taller node never leaves its drawer rows under the next card.
  void               ApplyLayoutToAdapter();
  void               SyncLayoutKey();
  void               PersistSavedSelection();
  void               ApplyLiveSelectionToAdapter();
  /**
   * @brief Reselect after draft removal removed @p removed_ids.
   *
   * Removes dead members, keeps the most recently selected survivor as primary,
   * and — when nothing survives — walks @p pre_removal_edges from
   * @p path_start_id (the topmost removed node) to the nearest surviving
   * downstream neighbor, then upstream, then the node at
   * @p first_removed_index in the post-removal list.
   */
  void               UpdateSelectionAfterRemoval(
                      const std::vector<NodeId>&                   removed_ids,
                      const std::vector<EditorNodeEdgeProjection>& pre_removal_edges,
                      const NodeId&                                path_start_id,
                      int                                          first_removed_index);
  [[nodiscard]] auto SessionLocationChanged() const -> bool;
  [[nodiscard]] auto SessionIdentityChanged() const -> bool;
  /// True when the bound session must not show a node graph (empty, loading, switch, or failed).
  [[nodiscard]] auto SessionHidesGraph() const -> bool;
  void               AdoptCommittedDocument(const PipelineDocument& document);
  [[nodiscard]] auto HasActiveGraph() const -> bool;
  void               SyncMaskThumbnails(const PipelineDocument& document);
  void               BindThumbnailGeometry();
  void               NotePhotographGeometry();

  QPointer<EditorSessionController>             session_;
  QPointer<AlcedoQanGraph>                      graph_adapter_;
  QPointer<EditorNodeLayoutStore>               layout_store_;
  QMetaObject::Connection                       state_connection_;
  QMetaObject::Connection                       history_connection_;
  QMetaObject::Connection                       availability_connection_;
  QMetaObject::Connection                       graph_adapter_connection_;
  QMetaObject::Connection                       layout_store_connection_;
  QMetaObject::Connection                       presentation_binding_connection_;
  QMetaObject::Connection                       presented_geometry_connection_;
  EditorNodeGraphSnapshot                       snapshot_{};
  bool                                          has_snapshot_ = false;
  alcedo::EditorMaskGroupSnapshot               mask_group_snapshot_{};
  bool                                          has_mask_group_snapshot_ = false;
  NodeId                                        selected_node_id_;
  /// Controller-owned selection in recency order. Invariant: empty iff
  /// selected_node_id_ is empty; otherwise it contains the primary node.
  std::vector<NodeId>                           selected_node_ids_;
  NodeId                                        last_selected_color_grade_id_;
  NodeId                                        selection_restore_node_id_;
  bool                                          command_active_            = false;
  bool                                          projection_apply_queued_   = false;
  bool                                          applying_layout_           = false;
  quint64                                       session_generation_        = 0;
  quint64                                       observed_history_revision_ = 0;
  quint64                                       projection_revision_       = 0;
  quint64                                       topology_revision_         = 0;
  quint64                                       element_id_                = 0;
  quint64                                       image_id_                  = 0;
  QString                                       version_id_;
  quint64                                       snapshot_element_id_ = 0;
  quint64                                       snapshot_image_id_   = 0;
  QString                                       snapshot_version_id_;
  QString                                       last_error_;
  std::unique_ptr<alcedo::EditorNodeGraphDraft> draft_;
  EditorNodeLayoutKey                           last_layout_key_{};
  quint64                                       adapter_attach_generation_            = 0;
  quint64                                       pending_apply_attach_generation_      = 0;
  int                                           queued_projection_apply_count_        = 0;
  int                                           completed_projection_apply_count_     = 0;
  int                                           skipped_stale_projection_apply_count_ = 0;
  MaskThumbnailCoordinator*                     mask_thumbnails_                      = nullptr;
};

void RegisterEditorNodeQmlTypes();

}  // namespace alcedo::ui
