//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_node_layout_store.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "ui/alcedo_main/album_backend/editor_node_controller.hpp"

namespace alcedo::ui {
namespace {

constexpr qreal kMinZoom = 0.1;
constexpr qreal kMaxZoom = 8.0;

}  // namespace

auto NodeIdToQString(const NodeId& node_id) -> QString {
  const auto value = node_id.Value();
  return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

auto NodeIdFromQString(const QString& node_id) -> NodeId { return NodeId{node_id.toStdString()}; }

EditorNodeLayoutStore::EditorNodeLayoutStore(QObject* parent)
    : EditorNodeLayoutStore(EditorNodeLayoutMetrics{}, parent) {}

EditorNodeLayoutStore::EditorNodeLayoutStore(EditorNodeLayoutMetrics metrics, QObject* parent)
    : QObject(parent), metrics_(metrics) {}

void EditorNodeLayoutStore::activate(const QString& project_id, quint64 element_id,
                                     quint64 image_id, const QString& version_id) {
  EditorNodeLayoutKey key;
  key.project_id = project_id;
  key.element_id = element_id;
  key.image_id   = image_id;
  key.version_id = version_id;
  if (key == current_key_) {
    return;
  }
  current_key_ = key;
  emit LayoutChanged();
}

auto EditorNodeLayoutStore::ClampPanelWidth(int width) const -> int {
  return std::clamp(width, metrics_.panel_width_min, metrics_.panel_width_max);
}

auto EditorNodeLayoutStore::ClampZoom(qreal zoom) const -> qreal {
  if (!std::isfinite(zoom)) {
    return 1.0;
  }
  return std::clamp(zoom, kMinZoom, kMaxZoom);
}

auto EditorNodeLayoutStore::MutableCurrent() -> EditorNodeLayoutValue& {
  auto it = values_.find(current_key_);
  if (it == values_.end()) {
    EditorNodeLayoutValue value;
    value.preferred_panel_width = metrics_.panel_width_default;
    value.zoom                  = 1.0;
    it                          = values_.emplace(current_key_, std::move(value)).first;
  }
  return it->second;
}

auto EditorNodeLayoutStore::CurrentOrDefault() const -> EditorNodeLayoutValue {
  const auto it = values_.find(current_key_);
  if (it == values_.end()) {
    EditorNodeLayoutValue value;
    value.preferred_panel_width = metrics_.panel_width_default;
    value.zoom                  = 1.0;
    return value;
  }
  return it->second;
}

auto EditorNodeLayoutStore::preferred_panel_width() const -> int {
  return CurrentOrDefault().preferred_panel_width;
}

void EditorNodeLayoutStore::set_preferred_panel_width(int width) {
  const int clamped = ClampPanelWidth(width);
  auto&     value   = MutableCurrent();
  if (value.preferred_panel_width == clamped) {
    return;
  }
  value.preferred_panel_width = clamped;
  emit LayoutChanged();
}

auto EditorNodeLayoutStore::zoom() const -> qreal { return CurrentOrDefault().zoom; }

void EditorNodeLayoutStore::set_zoom(qreal zoom) {
  const qreal clamped = ClampZoom(zoom);
  auto&       value   = MutableCurrent();
  if (qFuzzyCompare(value.zoom, clamped)) {
    return;
  }
  value.zoom = clamped;
  emit LayoutChanged();
}

auto EditorNodeLayoutStore::view_position() const -> QPointF {
  return CurrentOrDefault().view_position;
}

void EditorNodeLayoutStore::set_view_position(QPointF position) {
  auto& value = MutableCurrent();
  if (value.view_position == position) {
    return;
  }
  value.view_position = position;
  emit LayoutChanged();
}

auto EditorNodeLayoutStore::selected_node_id() const -> NodeId {
  return CurrentOrDefault().selected_node_id;
}

void EditorNodeLayoutStore::set_selected_node_id(NodeId node_id) {
  auto& value = MutableCurrent();
  if (value.selected_node_id == node_id) {
    return;
  }
  value.selected_node_id = std::move(node_id);
  emit LayoutChanged();
}

auto EditorNodeLayoutStore::selected_node_id_string() const -> QString {
  return NodeIdToQString(selected_node_id());
}

void EditorNodeLayoutStore::set_selected_node_id_string(const QString& node_id) {
  set_selected_node_id(NodeIdFromQString(node_id));
}

void EditorNodeLayoutStore::setNodePosition(const QString& node_id, qreal x, qreal y) {
  SetNodePosition(NodeIdFromQString(node_id), QPointF(x, y));
}

bool EditorNodeLayoutStore::hasNodePosition(const QString& node_id) const {
  return NodePosition(NodeIdFromQString(node_id)).has_value();
}

QPointF EditorNodeLayoutStore::nodePosition(const QString& node_id) const {
  return NodePosition(NodeIdFromQString(node_id)).value_or(QPointF());
}

void EditorNodeLayoutStore::setDrawerOpen(const QString& node_id, bool open) {
  SetDrawerOpen(NodeIdFromQString(node_id), open);
}

bool EditorNodeLayoutStore::drawerOpen(const QString& node_id) const {
  return DrawerOpen(NodeIdFromQString(node_id));
}

auto EditorNodeLayoutStore::NodePosition(const NodeId& node_id) const -> std::optional<QPointF> {
  const auto it = values_.find(current_key_);
  if (it == values_.end()) {
    return std::nullopt;
  }
  const auto pos = it->second.node_positions.find(node_id);
  if (pos == it->second.node_positions.end()) {
    return std::nullopt;
  }
  return pos->second;
}

auto EditorNodeLayoutStore::DrawerOpen(const NodeId& node_id) const -> bool {
  const auto it = values_.find(current_key_);
  if (it == values_.end()) {
    return true;
  }
  const auto flag = it->second.drawer_open.find(node_id);
  if (flag == it->second.drawer_open.end()) {
    return true;
  }
  return flag->second;
}

void EditorNodeLayoutStore::SetNodePosition(const NodeId& node_id, QPointF position) {
  if (node_id.Empty()) {
    return;
  }
  auto& value = MutableCurrent();
  auto  it    = value.node_positions.find(node_id);
  if (it != value.node_positions.end() && it->second == position) {
    return;
  }
  value.node_positions[node_id] = position;
  emit LayoutChanged();
}

void EditorNodeLayoutStore::SetDrawerOpen(const NodeId& node_id, bool open) {
  if (node_id.Empty()) {
    return;
  }
  auto& value = MutableCurrent();
  auto  it    = value.drawer_open.find(node_id);
  if (it != value.drawer_open.end() && it->second == open) {
    return;
  }
  value.drawer_open[node_id] = open;
  emit LayoutChanged();
}

auto EditorNodeLayoutStore::DefaultHeight(EditorNodeKind kind, int mask_count,
                                          bool drawer_open) const -> qreal {
  if (kind != EditorNodeKind::ColorGrade) {
    return static_cast<qreal>(metrics_.endpoint_height);
  }
  qreal height = static_cast<qreal>(metrics_.name_row_height + metrics_.divider_height +
                                    metrics_.drawer_header_height);
  if (drawer_open) {
    height += static_cast<qreal>(std::max(0, mask_count) * metrics_.mask_row_height);
  }
  return height;
}

void EditorNodeLayoutStore::EnsureDefaultPositions(const EditorNodeGraphSnapshot& snapshot) {
  qreal y     = static_cast<qreal>(metrics_.origin_y);
  bool  wrote = false;
  auto& value = MutableCurrent();
  for (const auto& node : snapshot.nodes) {
    const bool drawer_open = DrawerOpen(node.node_id);
    if (value.node_positions.find(node.node_id) == value.node_positions.end()) {
      value.node_positions[node.node_id] = QPointF(static_cast<qreal>(metrics_.origin_x), y);
      wrote                              = true;
    }
    y += DefaultHeight(node.node_kind, static_cast<int>(node.masks.size()), drawer_open) +
         static_cast<qreal>(metrics_.vertical_gap);
  }
  if (wrote) {
    emit LayoutChanged();
  }
}

void EditorNodeLayoutStore::AssignStagingPosition(const NodeId& node_id,
                                                  const EditorNodeGraphSnapshot& snapshot) {
  if (node_id.Empty()) {
    return;
  }
  auto& value = MutableCurrent();
  if (value.node_positions.find(node_id) != value.node_positions.end()) {
    return;
  }
  EnsureDefaultPositions(snapshot);

  const qreal column_x  = static_cast<qreal>(metrics_.origin_x);
  const qreal gap       = static_cast<qreal>(metrics_.vertical_gap);
  qreal       staging_y = static_cast<qreal>(metrics_.origin_y);
  auto        height_of = [this, &snapshot](const NodeId& id) -> qreal {
    for (const auto& node : snapshot.nodes) {
      if (node.node_id == id) {
        return DefaultHeight(node.node_kind, static_cast<int>(node.masks.size()), DrawerOpen(id));
      }
    }
    return DefaultHeight(EditorNodeKind::ColorGrade, 0, DrawerOpen(id));
  };
  for (const auto& [id, position] : value.node_positions) {
    if (id == node_id) {
      continue;
    }
    staging_y = std::max(staging_y, position.y() + height_of(id) + gap);
  }
  value.node_positions[node_id] = QPointF(column_x, staging_y);
  emit LayoutChanged();
}

void EditorNodeLayoutStore::ensureDefaultsFrom(QObject* controller) {
  auto* nodes = qobject_cast<EditorNodeController*>(controller);
  if (nodes == nullptr || !nodes->has_snapshot()) {
    return;
  }
  EnsureDefaultPositions(nodes->snapshot());
}

auto EditorNodeLayoutStore::Value(const EditorNodeLayoutKey& key) const -> EditorNodeLayoutValue {
  const auto it = values_.find(key);
  if (it == values_.end()) {
    EditorNodeLayoutValue value;
    value.preferred_panel_width = metrics_.panel_width_default;
    value.zoom                  = 1.0;
    return value;
  }
  return it->second;
}

}  // namespace alcedo::ui
