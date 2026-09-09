//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QColor>
#include <QPointF>
#include <QQuickItem>

#include <vector>

#include "ui/edit_viewer/edit_viewer_overlay_geometry.hpp"
#include "ui/edit_viewer/mask_overlay_geometry.hpp"

namespace alcedo::editor_rhi {

class EditorInteractionController;

// Pure geometry description used by both the QSG item and overlay tests.
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

// QQuickItem that renders crop mask/grid/handles, detail-ROI bounds, and Mask
// controls as retained QSGGeometryNode content. Photograph pixels stay in
// EditorViewportItem. The overlay accepts no pointer grab.
class EditorOverlayItem : public QQuickItem {
  Q_OBJECT
  Q_PROPERTY(EditorInteractionController* interaction READ interaction WRITE setInteraction NOTIFY
                 InteractionChanged)
  Q_PROPERTY(bool cropVisible READ cropVisible NOTIFY GeometryRevisionChanged)
  Q_PROPERTY(int geometryRevision READ geometryRevision NOTIFY GeometryRevisionChanged)
  // Diagnostics for tests: rebuilds coalesce; paint updates vertices in place.
  Q_PROPERTY(int geometryRebuildCount READ geometryRebuildCount NOTIFY GeometryRevisionChanged)
  Q_PROPERTY(int paintNodeCreateCount READ paintNodeCreateCount NOTIFY GeometryRevisionChanged)
  Q_PROPERTY(int maskPaintNodeCreateCount READ maskPaintNodeCreateCount NOTIFY
                 MaskOverlayRevisionChanged)
  Q_PROPERTY(QColor maskOverlayControlColor READ maskOverlayControlColor WRITE
                 setMaskOverlayControlColor NOTIFY MaskOverlayStyleChanged)
  Q_PROPERTY(QColor maskOverlayControlOutlineColor READ maskOverlayControlOutlineColor WRITE
                 setMaskOverlayControlOutlineColor NOTIFY MaskOverlayStyleChanged)
  Q_PROPERTY(QColor maskOverlayInactiveColor READ maskOverlayInactiveColor WRITE
                 setMaskOverlayInactiveColor NOTIFY MaskOverlayStyleChanged)
  Q_PROPERTY(qreal maskOverlayHandleRadius READ maskOverlayHandleRadius WRITE
                 setMaskOverlayHandleRadius NOTIFY MaskOverlayStyleChanged)
  Q_PROPERTY(qreal maskOverlayHandleOutlineWidth READ maskOverlayHandleOutlineWidth WRITE
                 setMaskOverlayHandleOutlineWidth NOTIFY MaskOverlayStyleChanged)
  Q_PROPERTY(qreal maskOverlayStrokeWidth READ maskOverlayStrokeWidth WRITE
                 setMaskOverlayStrokeWidth NOTIFY MaskOverlayStyleChanged)
  Q_PROPERTY(qreal maskOverlayAntialiasWidth READ maskOverlayAntialiasWidth WRITE
                 setMaskOverlayAntialiasWidth NOTIFY MaskOverlayStyleChanged)

 public:
  explicit EditorOverlayItem(QQuickItem* parent = nullptr);

  [[nodiscard]] auto interaction() const -> EditorInteractionController* { return interaction_; }
  void setInteraction(EditorInteractionController* controller);

  [[nodiscard]] auto cropVisible() const -> bool;
  [[nodiscard]] auto geometryRevision() const -> int { return geometry_revision_; }
  [[nodiscard]] auto geometryRebuildCount() const -> int { return geometry_rebuild_count_; }
  [[nodiscard]] auto paintNodeCreateCount() const -> int { return paint_node_create_count_; }
  [[nodiscard]] auto maskPaintNodeCreateCount() const -> int {
    return mask_paint_node_create_count_;
  }

  [[nodiscard]] auto maskOverlayControlColor() const -> QColor {
    return mask_style_.control_fill;
  }
  void setMaskOverlayControlColor(const QColor& color);
  [[nodiscard]] auto maskOverlayControlOutlineColor() const -> QColor {
    return mask_style_.control_outline;
  }
  void setMaskOverlayControlOutlineColor(const QColor& color);
  [[nodiscard]] auto maskOverlayInactiveColor() const -> QColor { return mask_style_.inactive; }
  void setMaskOverlayInactiveColor(const QColor& color);
  [[nodiscard]] auto maskOverlayHandleRadius() const -> qreal {
    return static_cast<qreal>(mask_style_.handle_radius_logical_px);
  }
  void setMaskOverlayHandleRadius(qreal radius);
  [[nodiscard]] auto maskOverlayHandleOutlineWidth() const -> qreal {
    return static_cast<qreal>(mask_style_.handle_outline_width_logical_px);
  }
  void setMaskOverlayHandleOutlineWidth(qreal width);
  [[nodiscard]] auto maskOverlayStrokeWidth() const -> qreal {
    return static_cast<qreal>(mask_style_.stroke_width_logical_px);
  }
  void setMaskOverlayStrokeWidth(qreal width);
  [[nodiscard]] auto maskOverlayAntialiasWidth() const -> qreal {
    return static_cast<qreal>(mask_style_.antialias_width_logical_px);
  }
  void setMaskOverlayAntialiasWidth(qreal width);

  // Test access: last built crop scene geometry after a sync.
  [[nodiscard]] auto lastSceneGeometry() const -> const OverlaySceneGeometry& {
    return last_scene_geometry_;
  }

  /**
   * @brief Publish mapped Mask controls for the next scene-graph frame.
   *
   * @param display Item-space handles and optional creation guides. Hidden
   *        clears Mask overlay nodes. Does not lock the pipeline or read
   *        document/cache state.
   *
   * Thread: GUI. Schedules @c update(). @c updatePaintNode copies the derived
   * triangles while Qt blocks the GUI thread.
   */
  void setMaskOverlayDisplay(MaskOverlayDisplay display);
  [[nodiscard]] auto maskOverlayDisplay() const -> const MaskOverlayDisplay& {
    return mask_display_;
  }
  [[nodiscard]] auto lastMaskSceneGeometry() const -> const MaskOverlaySceneGeometry& {
    return last_mask_scene_geometry_;
  }
  [[nodiscard]] auto maskOverlayStyle() const -> const MaskOverlayStyle& { return mask_style_; }

  // Force a rebuild from the current interaction snapshot (also used by tests).
  Q_INVOKABLE void refreshFromInteraction();

 signals:
  void InteractionChanged();
  void GeometryRevisionChanged();
  void MaskOverlayRevisionChanged();
  void MaskOverlayStyleChanged();

 protected:
  auto updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) -> QSGNode* override;
  void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

 private:
  struct OverlayRootNode;
  void scheduleRebuildFromInteraction();
  void rebuildSceneGeometry();
  void rebuildMaskSceneGeometry();
  void bindInteraction(EditorInteractionController* controller);
  void onInteractionOverlayChanged();

  EditorInteractionController* interaction_ = nullptr;
  OverlaySceneGeometry last_scene_geometry_{};
  MaskOverlayDisplay mask_display_{};
  MaskOverlayStyle mask_style_ = DefaultMaskOverlayStyle();
  MaskOverlaySceneGeometry last_mask_scene_geometry_{};
  int geometry_revision_ = 0;
  int geometry_rebuild_count_ = 0;
  int paint_node_create_count_ = 0;
  int mask_paint_node_create_count_ = 0;
  bool geometry_dirty_ = true;
  bool rebuild_scheduled_ = false;
};

}  // namespace alcedo::editor_rhi
