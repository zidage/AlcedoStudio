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
#include <optional>

#include "app/editor_node_graph_projection.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"

namespace alcedo::ui {

class EditorSessionController;

/**
 * @brief Application-layer owner of Nodes-page product selection and projection.
 *
 * Owns the session generation, selected NodeId, and published
 * EditorNodeGraphSnapshot. It does not own Qan visuals or layout coordinates.
 * Graph mutations (Add, Rename, Delete, Reconnect) are not exposed here.
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
  Q_PROPERTY(quint64 sessionGeneration READ session_generation NOTIFY SnapshotChanged)
  Q_PROPERTY(quint64 projectionRevision READ projection_revision NOTIFY SnapshotChanged)
  Q_PROPERTY(quint64 topologyRevision READ topology_revision NOTIFY SnapshotChanged)
  Q_PROPERTY(quint64 elementId READ element_id NOTIFY SnapshotChanged)
  Q_PROPERTY(quint64 imageId READ image_id NOTIFY SnapshotChanged)
  Q_PROPERTY(QString versionId READ version_id NOTIFY SnapshotChanged)
  Q_PROPERTY(QString lastError READ last_error NOTIFY lastErrorChanged)
  Q_PROPERTY(bool hasSnapshot READ has_snapshot NOTIFY SnapshotChanged)
  Q_PROPERTY(bool canAddColorGrade READ can_add_color_grade NOTIFY SnapshotChanged)

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
  Q_INVOKABLE bool   refreshFromSession();

  /**
   * @brief Project the current snapshot onto an AlcedoQanGraph adapter.
   * @param adapter AlcedoQanGraph instance. Rejects null, wrong types, and a
   *        missing snapshot. On success, applies the one-node product selection.
   * @return false when ApplySnapshot fails; lastError holds the adapter error.
   */
  Q_INVOKABLE bool   applyToGraph(QObject* adapter);

  /**
   * @brief Select one product node.
   *
   * Unknown or empty ids fail closed and leave the current selection unchanged.
   * @param node_id Product NodeId string.
   */
  Q_INVOKABLE void   selectNode(const QString& node_id);
  Q_INVOKABLE void   selectPreviousBackboneNode();
  Q_INVOKABLE void   selectNextBackboneNode();
  Q_INVOKABLE void   selectDevelop();
  Q_INVOKABLE void   selectDrt();

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
  [[nodiscard]] auto can_add_color_grade() const -> bool { return false; }
  [[nodiscard]] auto snapshot() const -> const EditorNodeGraphSnapshot& { return snapshot_; }

  /**
   * @brief Set image/Version identity used by layout keys when no session is bound.
   */
  void               SetLayoutIdentity(quint64 element_id, quint64 image_id, QString version_id);

 signals:
  void EditorSessionChanged();
  void SnapshotChanged();
  void SelectionChanged();
  void lastErrorChanged();

 private:
  void               DisconnectSession();
  void               OnSessionChanged();
  void               ClearSnapshot();
  void               SetLastError(QString error);
  void               RestoreSelectionAfterSnapshot();
  [[nodiscard]] auto ContainsNode(const NodeId& node_id) const -> bool;
  [[nodiscard]] auto DefaultSelectedNodeId() const -> NodeId;
  [[nodiscard]] auto IndexOf(const NodeId& node_id) const -> int;
  [[nodiscard]] auto TopologyChanged(const EditorNodeGraphSnapshot& snapshot) const -> bool;
  [[nodiscard]] auto BoundSessionGeneration() const -> std::optional<std::uint64_t>;
  void               SelectByKind(EditorNodeKind kind);
  void               SelectAt(int index);

  QPointer<EditorSessionController> session_;
  QMetaObject::Connection           state_connection_;
  QMetaObject::Connection           history_connection_;
  EditorNodeGraphSnapshot           snapshot_{};
  bool                              has_snapshot_ = false;
  NodeId                            selected_node_id_;
  quint64                           session_generation_  = 0;
  quint64                           projection_revision_ = 0;
  quint64                           topology_revision_   = 0;
  quint64                           element_id_          = 0;
  quint64                           image_id_            = 0;
  QString                           version_id_;
  QString                           last_error_;
};

void RegisterEditorNodeQmlTypes();

}  // namespace alcedo::ui
