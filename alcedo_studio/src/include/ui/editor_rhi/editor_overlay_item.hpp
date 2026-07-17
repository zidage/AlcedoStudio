//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QPointF>
#include <QQuickItem>

#include <vector>

#include "ui/edit_viewer/edit_viewer_overlay_geometry.hpp"

namespace alcedo::editor_rhi {

class EditorInteractionController;

// Pure geometry description used by both the QSG item and golden tests.
// Vertices are in item/logical coordinates (not physical pixels).
struct OverlaySceneGeometry {
  // Triangle list (groups of 3) for the dim mask outside the crop.
  std::vector<QPointF> mask_triangles;
  // Line pairs for crop border.
  std::vector<QPointF> border_lines;
  // Line pairs for edge grips (thick outer then thin inner drawn separately).
  std::vector<QPointF> edge_grip_lines;
  // Rule-of-thirds line pairs.
  std::vector<QPointF> grid_lines;
  // Rotate stem as a single line pair.
  std::vector<QPointF> rotate_stem_line;
  // Filled handle discs as triangle fans (center, rim...). Each handle is
  // recorded as: center, then N rim points (closed by repeating first rim).
  std::vector<QPointF> handle_discs;
  int handle_segments = 0;
  int handle_count = 0;
  // Detail ROI bounds line pairs.
  std::vector<QPointF> detail_roi_lines;
  bool has_crop = false;
  bool has_detail_roi = false;
};

// Builds retained overlay draw primitives from logical geometry. Pure and
// testable without a scene graph or GPU.
auto BuildOverlaySceneGeometry(const CropOverlayWidgetGeometry& geometry,
                               bool crop_tool_visible) -> OverlaySceneGeometry;

// QQuickItem that renders crop mask/grid/handles and detail-ROI bounds as
// retained QSGGeometryNode content. Photograph pixels stay in EditorViewportItem.
class EditorOverlayItem : public QQuickItem {
  Q_OBJECT
  Q_PROPERTY(EditorInteractionController* interaction READ interaction WRITE setInteraction NOTIFY
                 InteractionChanged)
  Q_PROPERTY(bool cropVisible READ cropVisible NOTIFY GeometryRevisionChanged)
  Q_PROPERTY(int geometryRevision READ geometryRevision NOTIFY GeometryRevisionChanged)

 public:
  explicit EditorOverlayItem(QQuickItem* parent = nullptr);

  [[nodiscard]] auto interaction() const -> EditorInteractionController* { return interaction_; }
  void setInteraction(EditorInteractionController* controller);

  [[nodiscard]] auto cropVisible() const -> bool;
  [[nodiscard]] auto geometryRevision() const -> int { return geometry_revision_; }

  // Test access: last built scene geometry after a sync.
  [[nodiscard]] auto lastSceneGeometry() const -> const OverlaySceneGeometry& {
    return last_scene_geometry_;
  }

  // Force a rebuild from the current interaction snapshot (also used by tests).
  Q_INVOKABLE void refreshFromInteraction();

 signals:
  void InteractionChanged();
  void GeometryRevisionChanged();

 protected:
  auto updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) -> QSGNode* override;
  void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

 private:
  struct OverlayRootNode;
  void rebuildSceneGeometry();
  void bindInteraction(EditorInteractionController* controller);
  void onInteractionOverlayChanged();

  EditorInteractionController* interaction_ = nullptr;
  OverlaySceneGeometry last_scene_geometry_{};
  int geometry_revision_ = 0;
  bool geometry_dirty_ = true;
};

}  // namespace alcedo::editor_rhi
