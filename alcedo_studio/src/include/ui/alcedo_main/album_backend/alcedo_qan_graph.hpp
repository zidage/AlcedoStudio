//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QMetaObject>
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QQuickItem>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <cstdint>
#include <map>
#include <memory>
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
  Q_PROPERTY(QUrl colorGradeDelegateUrl READ color_grade_delegate_url WRITE
                 set_color_grade_delegate_url NOTIFY DelegatesChanged)
  Q_PROPERTY(QUrl endpointDelegateUrl READ endpoint_delegate_url WRITE set_endpoint_delegate_url
                 NOTIFY DelegatesChanged)
  Q_PROPERTY(QUrl portDelegateUrl READ port_delegate_url WRITE set_port_delegate_url NOTIFY
                 DelegatesChanged)
  Q_PROPERTY(QUrl portDockDelegateUrl READ port_dock_delegate_url WRITE
                 set_port_dock_delegate_url NOTIFY DelegatesChanged)
  Q_PROPERTY(QUrl edgeDelegateUrl READ edge_delegate_url WRITE set_edge_delegate_url NOTIFY
                 DelegatesChanged)

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
   * @brief Set the QML delegate URL for Color Grade nodes.
   *
   * The URL is resolved against the bound graph's QQmlEngine. An empty URL or a
   * load error fails ApplySnapshot with the real QML error. There is no fallback
   * to the default QuickQanava node delegate.
   */
  void               set_color_grade_delegate_url(const QUrl& url);
  [[nodiscard]] auto color_grade_delegate_url() const -> QUrl;

  /**
   * @brief Set the QML delegate URL for Develop and DRT/Post nodes.
   * @see set_color_grade_delegate_url
   */
  void               set_endpoint_delegate_url(const QUrl& url);
  [[nodiscard]] auto endpoint_delegate_url() const -> QUrl;

  /**
   * @brief Set the QML delegate URL for scene-image ports.
   *
   * Each bound Qan graph receives its own component instance because
   * qan::Graph takes ownership of portDelegate.
   */
  void               set_port_delegate_url(const QUrl& url);
  [[nodiscard]] auto port_delegate_url() const -> QUrl;

  /**
   * @brief Set the QML delegate URL for the horizontal port dock.
   *
   * The dock positions port items against the node card edge. Each bound Qan
   * graph receives its own component instance because qan::Graph takes
   * ownership of horizontalDockDelegate.
   */
  void               set_port_dock_delegate_url(const QUrl& url);
  [[nodiscard]] auto port_dock_delegate_url() const -> QUrl;

  /**
   * @brief Set the QML delegate URL for backbone edges.
   * @see set_color_grade_delegate_url
   */
  void               set_edge_delegate_url(const QUrl& url);
  [[nodiscard]] auto edge_delegate_url() const -> QUrl;

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

  /**
   * @brief Apply the one-node product selection to live Qan visuals.
   *
   * Clears the Qan selected-node list, then selects at most one mapped node.
   * A Qan selected-node list is never treated as a product selection source.
   */
  void ApplyProductSelection(const std::optional<NodeId>& node_id);

  /**
   * @brief Move a live node item. No-op for unknown or stale NodeIds.
   */
  void SetNodeItemPosition(const NodeId& node_id, QPointF position);
  [[nodiscard]] auto NodeItemPosition(const NodeId& node_id) const -> std::optional<QPointF>;

  /**
   * @brief Set Color Grade Mask-drawer open state on the live delegate.
   *
   * Endpoints ignore the call. Missing NodeIds are no-ops.
   */
  void SetDrawerOpen(const NodeId& node_id, bool open);
  [[nodiscard]] auto DrawerOpen(const NodeId& node_id) const -> bool;

  Q_INVOKABLE QString liveNodeId(QObject* node) const;
  Q_INVOKABLE void    setNodePosition(const QString& node_id, qreal x, qreal y);
  Q_INVOKABLE QPointF nodePosition(const QString& node_id) const;
  Q_INVOKABLE void    setDrawerOpen(const QString& node_id, bool open);
  Q_INVOKABLE bool    drawerOpen(const QString& node_id) const;
  Q_INVOKABLE void    applyProductSelection(const QString& node_id);

 signals:
  void GraphChanged();
  void DelegatesChanged();
  /**
   * @brief Emitted when a Color Grade Mask drawer is opened or closed.
   *
   * Local UI state only. Listeners must not write PipelineDocument or start a
   * photo render.
   */
  void NodeDrawerOpenChanged(const QString& nodeId, bool open);

 protected:
  /**
   * @brief Documented @c qan::Graph::insertNode() entry used during rebuilds.
   *
   * Uses the Color Grade or endpoint delegate for @p node. Tests may override
   * this to inject a nullptr result. Production always forwards to the bound
   * graph with the matching Alcedo delegate.
   */
  [[nodiscard]] virtual auto InsertQanNode(qan::Graph& graph, const EditorNodeProjection& node)
      -> qan::Node*;

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
  void               ClearDrawerConnections();
  void               BindDrawerSignals();
  void               BindDrawerSignal(QQuickItem* item, const NodeId& node_id);
  void               ConfigureGraphPolicy();
  void               OnGraphDestroyed();

 private slots:
  void OnDrawerOpenChanged();

 private:
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
  void               ApplyNodePresentation(qan::Node& qan_node, const EditorNodeProjection& node);
  [[nodiscard]] auto EnsureDelegates() -> QString;
  struct LoadedComponent {
    std::unique_ptr<QQmlComponent> component;
    QString                        error;
  };
  [[nodiscard]] auto        LoadComponent(const QUrl& url, const QString& role) -> LoadedComponent;
  void                      DropCachedDelegates();
  [[nodiscard]] auto        InstallPortDelegate() -> QString;
  [[nodiscard]] auto        InstallPortDockDelegate() -> QString;
  void                      InstallInvisibleSelectionDelegate();
  [[nodiscard]] auto        ComponentFor(EditorNodeKind kind) const -> QQmlComponent*;

  [[nodiscard]] static auto MakeEdgeKey(const EditorNodeEdgeProjection& edge) -> EdgeKey;
  [[nodiscard]] static auto ToQString(std::string_view text) -> QString;
  [[nodiscard]] static auto QanPortId(bool is_input, const PortId& port_id) -> QString;
  [[nodiscard]] static auto NodeKindKey(EditorNodeKind kind) -> QString;
  [[nodiscard]] static auto SourceKindKey(MaskSourceKind kind) -> QString;
  [[nodiscard]] static auto MasksToVariant(const std::vector<EditorNodeMaskProjection>& masks)
      -> QVariantList;

  QPointer<qan::Graph>                              graph_;
  QUrl                                              color_grade_delegate_url_;
  QUrl                                              endpoint_delegate_url_;
  QUrl                                              port_delegate_url_;
  QUrl                                              port_dock_delegate_url_;
  QUrl                                              edge_delegate_url_;
  QPointer<QQmlEngine>                              delegate_engine_;
  QPointer<qan::Graph>                              port_delegate_graph_;
  QPointer<qan::Graph>                              port_dock_delegate_graph_;
  std::unique_ptr<QQmlComponent>                    color_grade_component_;
  std::unique_ptr<QQmlComponent>                    endpoint_component_;
  std::unique_ptr<QQmlComponent>                    edge_component_;
  bool                                              has_projection_      = false;
  bool                                              rebuild_in_progress_ = false;
  EditorNodeGraphSnapshot                           applied_;
  std::map<NodeId, QPointer<qan::Node>>             node_by_id_;
  std::map<EdgeKey, QPointer<qan::Edge>>            edge_by_key_;
  std::map<NodeId, NodePorts>                       ports_by_node_;
  std::map<NodeId, EditorNodeProjection>            node_projections_;
  std::unordered_map<const qan::Node*, ReverseNode> node_from_qan_;
  std::vector<QMetaObject::Connection>              drawer_connections_;
  NodeId                                            product_selected_node_id_;
};

}  // namespace alcedo::ui
