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
// All stroke content is triangle lists so D3D11 (no reliable lineWidth) matches
// OpenGL and the legacy QPainter overlay appearance.
struct OverlaySceneGeometry {
  // Triangle list (groups of 3) for the dim mask outside the crop (hole-safe).
  std::vector<QPointF> mask_triangles;
  // Outer then inner crop border strokes as triangle lists.
  std::vector<QPointF> border_outer_triangles;
  std::vector<QPointF> border_inner_triangles;
  // Edge grip strokes (outer thick, inner thin).
  std::vector<QPointF> grip_outer_triangles;
  std::vector<QPointF> grip_inner_triangles;
  // Rule-of-thirds dashed strokes.
  std::vector<QPointF> grid_triangles;
  // Rotate stem strokes.
  std::vector<QPointF> stem_outer_triangles;
  std::vector<QPointF> stem_inner_triangles;
  // Handle discs: dark outline ring + white fill as triangle fans expanded.
  std::vector<QPointF> handle_outline_triangles;
  std::vector<QPointF> handle_fill_triangles;
  int handle_count = 0;
  // Detail ROI bounds stroke.
  std::vector<QPointF> detail_roi_triangles;
  bool has_crop = false;
  bool has_detail_roi = false;

  // Backward-compatible aliases used by earlier tests (border line pairs count).
  // Prefer the triangle fields above.
  [[nodiscard]] auto border_lines_empty() const -> bool {
    return border_outer_triangles.empty();
  }
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
  // Diagnostics for tests: rebuilds coalesce; paint updates vertices in place.
  Q_PROPERTY(int geometryRebuildCount READ geometryRebuildCount NOTIFY GeometryRevisionChanged)
  Q_PROPERTY(int paintNodeCreateCount READ paintNodeCreateCount NOTIFY GeometryRevisionChanged)

 public:
  explicit EditorOverlayItem(QQuickItem* parent = nullptr);

  [[nodiscard]] auto interaction() const -> EditorInteractionController* { return interaction_; }
  void setInteraction(EditorInteractionController* controller);

  [[nodiscard]] auto cropVisible() const -> bool;
  [[nodiscard]] auto geometryRevision() const -> int { return geometry_revision_; }
  [[nodiscard]] auto geometryRebuildCount() const -> int { return geometry_rebuild_count_; }
  [[nodiscard]] auto paintNodeCreateCount() const -> int { return paint_node_create_count_; }

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
  void scheduleRebuildFromInteraction();
  void rebuildSceneGeometry();
  void bindInteraction(EditorInteractionController* controller);
  void onInteractionOverlayChanged();

  EditorInteractionController* interaction_ = nullptr;
  OverlaySceneGeometry last_scene_geometry_{};
  int geometry_revision_ = 0;
  int geometry_rebuild_count_ = 0;
  int paint_node_create_count_ = 0;
  bool geometry_dirty_ = true;
  bool rebuild_scheduled_ = false;
};

}  // namespace alcedo::editor_rhi
