//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QPointF>
#include <QString>
#include <cstdint>
#include <map>
#include <optional>

#include "app/editor_node_graph_projection.hpp"
#include "edit/graph/graph_ids.hpp"

namespace alcedo::ui {

/**
 * @brief Identity of one Nodes-page local layout.
 *
 * Project, image, and Version identities are copied strings and integers. The
 * store never holds QObject, qan::Node, or QML item pointers.
 */
struct EditorNodeLayoutKey {
  QString     project_id;
  quint64     element_id = 0;
  quint64     image_id   = 0;
  QString     version_id;

  friend auto operator<(const EditorNodeLayoutKey& lhs, const EditorNodeLayoutKey& rhs) -> bool {
    if (lhs.project_id != rhs.project_id) {
      return lhs.project_id < rhs.project_id;
    }
    if (lhs.element_id != rhs.element_id) {
      return lhs.element_id < rhs.element_id;
    }
    if (lhs.image_id != rhs.image_id) {
      return lhs.image_id < rhs.image_id;
    }
    return lhs.version_id < rhs.version_id;
  }

  friend auto operator==(const EditorNodeLayoutKey& lhs, const EditorNodeLayoutKey& rhs) -> bool {
    return lhs.project_id == rhs.project_id && lhs.element_id == rhs.element_id &&
           lhs.image_id == rhs.image_id && lhs.version_id == rhs.version_id;
  }
};

/**
 * @brief Geometry tokens used to compute the first backbone layout.
 *
 * Production fills these from AppTheme. Tests may supply explicit values.
 */
struct EditorNodeLayoutMetrics {
  int origin_x             = 48;
  int origin_y             = 48;
  int vertical_gap         = 48;
  int node_width           = 220;
  int endpoint_height      = 40;
  int name_row_height      = 32;
  int drawer_header_height = 28;
  int mask_row_height      = 28;
  int divider_height       = 1;
  int panel_width_min      = 260;
  int panel_width_max      = 460;
  int panel_width_default  = 320;
};

/**
 * @brief Local UI values for one Nodes-page layout key.
 *
 * Node positions and Mask-drawer open flags are retained after a NodeId leaves
 * the live graph so Undo can restore the prior layout. This store does not
 * write PipelineDocument or history.
 */
struct EditorNodeLayoutValue {
  int                       preferred_panel_width = 320;
  QPointF                   view_position;
  qreal                     zoom = 1.0;
  NodeId                    selected_node_id;
  std::map<NodeId, QPointF> node_positions;
  std::map<NodeId, bool>    drawer_open;
};

/**
 * @brief Owns Nodes-page local layout keyed by project, image, and Version.
 *
 * Threading: GUI thread only. Side effects: in-memory layout values. No history
 * commit and no photo render.
 */
class EditorNodeLayoutStore : public QObject {
  Q_OBJECT
  Q_PROPERTY(int preferredPanelWidth READ preferred_panel_width WRITE set_preferred_panel_width
                 NOTIFY LayoutChanged)
  Q_PROPERTY(qreal zoom READ zoom WRITE set_zoom NOTIFY LayoutChanged)
  Q_PROPERTY(QPointF viewPosition READ view_position WRITE set_view_position NOTIFY LayoutChanged)
  Q_PROPERTY(QString selectedNodeId READ selected_node_id_string WRITE set_selected_node_id_string
                 NOTIFY LayoutChanged)

 public:
  explicit EditorNodeLayoutStore(QObject* parent = nullptr);
  explicit EditorNodeLayoutStore(EditorNodeLayoutMetrics metrics, QObject* parent = nullptr);

  /**
   * @brief Fill metrics from the process AppTheme tokens.
   * @pre AppTheme has been constructed.
   */
  [[nodiscard]] static auto MetricsFromAppTheme() -> EditorNodeLayoutMetrics;

  /**
   * @brief Select the layout key used by the QML-facing getters and setters.
   *
   * A new key starts with default zoom 1, origin view position, default panel
   * width, and no stored node positions. Missing drawer flags read as open.
   */
  Q_INVOKABLE void    activate(const QString& project_id, quint64 element_id, quint64 image_id,
                               const QString& version_id);

  [[nodiscard]] auto  current_key() const -> EditorNodeLayoutKey { return current_key_; }
  [[nodiscard]] auto  metrics() const -> const EditorNodeLayoutMetrics& { return metrics_; }

  [[nodiscard]] auto  preferred_panel_width() const -> int;
  void                set_preferred_panel_width(int width);

  [[nodiscard]] auto  zoom() const -> qreal;
  void                set_zoom(qreal zoom);

  [[nodiscard]] auto  view_position() const -> QPointF;
  void                set_view_position(QPointF position);

  [[nodiscard]] auto  selected_node_id() const -> NodeId;
  void                set_selected_node_id(NodeId node_id);
  [[nodiscard]] auto  selected_node_id_string() const -> QString;
  void                set_selected_node_id_string(const QString& node_id);

  /**
   * @brief Store a user-moved node position for the current key.
   *
   * Positions for a NodeId are kept even after that node leaves the snapshot.
   */
  Q_INVOKABLE void    setNodePosition(const QString& node_id, qreal x, qreal y);
  Q_INVOKABLE bool    hasNodePosition(const QString& node_id) const;
  Q_INVOKABLE QPointF nodePosition(const QString& node_id) const;

  /**
   * @brief Store Mask-drawer open state for the current key.
   *
   * Missing entries read as open. Remove and Undo do not erase this value.
   */
  Q_INVOKABLE void    setDrawerOpen(const QString& node_id, bool open);
  Q_INVOKABLE bool    drawerOpen(const QString& node_id) const;

  [[nodiscard]] auto  NodePosition(const NodeId& node_id) const -> std::optional<QPointF>;
  [[nodiscard]] auto  DrawerOpen(const NodeId& node_id) const -> bool;
  void                SetNodePosition(const NodeId& node_id, QPointF position);
  void                SetDrawerOpen(const NodeId& node_id, bool open);

  /**
   * @brief Assign deterministic backbone positions for NodeIds that have none.
   *
   * Existing stored positions are left unchanged. Drawer flags are not created.
   * @param snapshot Current immutable projection. Only its node order and Mask
   *        counts affect newly assigned positions.
   */
  void                EnsureDefaultPositions(const EditorNodeGraphSnapshot& snapshot);

  /**
   * @brief QML entry that copies defaults from an EditorNodeController snapshot.
   * @param controller EditorNodeController, or ignored when the type does not match.
   */
  Q_INVOKABLE void    ensureDefaultsFrom(QObject* controller);

  /**
   * @return Copied value for @p key, or a default-constructed value when absent.
   */
  [[nodiscard]] auto  Value(const EditorNodeLayoutKey& key) const -> EditorNodeLayoutValue;

  /**
   * @brief Height of one node in the first deterministic layout.
   */
  [[nodiscard]] auto  DefaultHeight(EditorNodeKind kind, int mask_count, bool drawer_open) const
      -> qreal;

 signals:
  void LayoutChanged();

 private:
  [[nodiscard]] auto      MutableCurrent() -> EditorNodeLayoutValue&;
  [[nodiscard]] auto      CurrentOrDefault() const -> EditorNodeLayoutValue;
  [[nodiscard]] auto      ClampPanelWidth(int width) const -> int;
  [[nodiscard]] auto      ClampZoom(qreal zoom) const -> qreal;

  EditorNodeLayoutMetrics metrics_{};
  EditorNodeLayoutKey     current_key_{};
  std::map<EditorNodeLayoutKey, EditorNodeLayoutValue> values_;
};

[[nodiscard]] auto NodeIdToQString(const NodeId& node_id) -> QString;
[[nodiscard]] auto NodeIdFromQString(const QString& node_id) -> NodeId;

}  // namespace alcedo::ui
