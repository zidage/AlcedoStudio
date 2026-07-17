//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/editor_rhi/editor_overlay_item.hpp"

#include "ui/editor_rhi/editor_interaction_controller.hpp"

#include <QPainterPath>
#include <QPolygonF>
#include <QSGVertexColorMaterial>

#include <algorithm>
#include <cmath>

#include "ui/edit_viewer/crop_geometry.hpp"

namespace alcedo::editor_rhi {
namespace {

constexpr int kHandleSegments = 14;
constexpr float kMaskAlpha = 112.0f / 255.0f;

void AppendLinePair(std::vector<QPointF>& lines, const QPointF& a, const QPointF& b) {
  lines.push_back(a);
  lines.push_back(b);
}

void AppendDisc(std::vector<QPointF>& discs, const QPointF& center, float radius, int segments) {
  discs.push_back(center);
  for (int i = 0; i <= segments; ++i) {
    const float angle = (static_cast<float>(i) / static_cast<float>(segments)) * 6.28318530718f;
    discs.emplace_back(center.x() + (std::cos(angle) * radius),
                       center.y() + (std::sin(angle) * radius));
  }
}

void AppendPolygonLines(std::vector<QPointF>& lines, const std::array<QPointF, 4>& corners) {
  for (size_t i = 0; i < corners.size(); ++i) {
    AppendLinePair(lines, corners[i], corners[(i + 1) % corners.size()]);
  }
}

// Fan-triangulate a simple polygon (assumes nearly convex outer ring).
void AppendFanTriangles(std::vector<QPointF>& triangles, const QPolygonF& poly) {
  if (poly.size() < 3) {
    return;
  }
  const QPointF origin = poly.at(0);
  for (int i = 1; i + 1 < poly.size(); ++i) {
    triangles.push_back(origin);
    triangles.push_back(poly.at(i));
    triangles.push_back(poly.at(i + 1));
  }
}

void FillColoredGeometry(QSGGeometry* geometry, const std::vector<QPointF>& points,
                         const QColor& color, QSGGeometry::DrawingMode mode) {
  geometry->allocate(static_cast<int>(points.size()));
  geometry->setDrawingMode(mode);
  auto* vertices = geometry->vertexDataAsColoredPoint2D();
  const uchar r = static_cast<uchar>(color.red());
  const uchar g = static_cast<uchar>(color.green());
  const uchar b = static_cast<uchar>(color.blue());
  const uchar a = static_cast<uchar>(color.alpha());
  for (size_t i = 0; i < points.size(); ++i) {
    vertices[i].set(static_cast<float>(points[i].x()), static_cast<float>(points[i].y()), r, g, b,
                    a);
  }
}

auto MakeColoredNode(const std::vector<QPointF>& points, const QColor& color,
                     QSGGeometry::DrawingMode mode, float line_width = 1.0f) -> QSGGeometryNode* {
  if (points.empty()) {
    return nullptr;
  }
  auto* node = new QSGGeometryNode;
  auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(),
                                   static_cast<int>(points.size()));
  geometry->setLineWidth(line_width);
  FillColoredGeometry(geometry, points, color, mode);
  node->setGeometry(geometry);
  node->setFlag(QSGNode::OwnsGeometry);

  auto* material = new QSGVertexColorMaterial;
  node->setMaterial(material);
  node->setFlag(QSGNode::OwnsMaterial);
  return node;
}

// Disc triangle list from center + rim points.
auto ExpandHandleDiscs(const OverlaySceneGeometry& scene) -> std::vector<QPointF> {
  std::vector<QPointF> triangles;
  if (scene.handle_count <= 0 || scene.handle_segments <= 0) {
    return triangles;
  }
  const int verts_per = 1 + scene.handle_segments + 1;
  triangles.reserve(static_cast<size_t>(scene.handle_count * scene.handle_segments * 3));
  for (int h = 0; h < scene.handle_count; ++h) {
    const int base = h * verts_per;
    if (base + verts_per > static_cast<int>(scene.handle_discs.size())) {
      break;
    }
    const QPointF center = scene.handle_discs[static_cast<size_t>(base)];
    for (int s = 0; s < scene.handle_segments; ++s) {
      triangles.push_back(center);
      triangles.push_back(scene.handle_discs[static_cast<size_t>(base + 1 + s)]);
      triangles.push_back(scene.handle_discs[static_cast<size_t>(base + 2 + s)]);
    }
  }
  return triangles;
}

}  // namespace

auto BuildOverlaySceneGeometry(const CropOverlayWidgetGeometry& geometry, bool crop_tool_visible)
    -> OverlaySceneGeometry {
  OverlaySceneGeometry scene;
  scene.handle_segments = kHandleSegments;

  if (geometry.detail_roi_valid) {
    scene.has_detail_roi = true;
    AppendPolygonLines(scene.detail_roi_lines, geometry.detail_roi_corners_widget);
  }

  if (!crop_tool_visible || !geometry.image_rect_valid || !geometry.crop_corners_valid) {
    return scene;
  }
  scene.has_crop = true;

  QPolygonF crop_polygon;
  crop_polygon.reserve(4);
  for (const auto& corner : geometry.crop_corners_widget) {
    crop_polygon << corner;
  }

  QPainterPath image_path;
  image_path.addRect(geometry.image_rect);
  QPainterPath crop_path;
  crop_path.addPolygon(crop_polygon);
  crop_path.closeSubpath();
  const QPainterPath mask_path = image_path.subtracted(crop_path);
  const QList<QPolygonF> mask_polys = mask_path.toFillPolygons();
  for (const QPolygonF& poly : mask_polys) {
    AppendFanTriangles(scene.mask_triangles, poly);
  }

  AppendPolygonLines(scene.border_lines, geometry.crop_corners_widget);

  auto append_edge_grip = [&](const QPointF& a, const QPointF& b) {
    const QPointF grip_a = CropGeometry::LerpPoint(a, b, 0.38f);
    const QPointF grip_b = CropGeometry::LerpPoint(a, b, 0.62f);
    AppendLinePair(scene.edge_grip_lines, grip_a, grip_b);
  };
  append_edge_grip(geometry.crop_corners_widget[0], geometry.crop_corners_widget[1]);
  append_edge_grip(geometry.crop_corners_widget[1], geometry.crop_corners_widget[2]);
  append_edge_grip(geometry.crop_corners_widget[2], geometry.crop_corners_widget[3]);
  append_edge_grip(geometry.crop_corners_widget[3], geometry.crop_corners_widget[0]);

  for (const float t : {1.0f / 3.0f, 2.0f / 3.0f}) {
    AppendLinePair(scene.grid_lines,
                   CropGeometry::LerpPoint(geometry.crop_corners_widget[0],
                                           geometry.crop_corners_widget[1], t),
                   CropGeometry::LerpPoint(geometry.crop_corners_widget[3],
                                           geometry.crop_corners_widget[2], t));
    AppendLinePair(scene.grid_lines,
                   CropGeometry::LerpPoint(geometry.crop_corners_widget[0],
                                           geometry.crop_corners_widget[3], t),
                   CropGeometry::LerpPoint(geometry.crop_corners_widget[1],
                                           geometry.crop_corners_widget[2], t));
  }

  AppendLinePair(scene.rotate_stem_line, geometry.rotate_stem_widget, geometry.rotate_handle_widget);

  for (const auto& corner : geometry.crop_corners_widget) {
    AppendDisc(scene.handle_discs, corner, CropGeometry::kCropCornerDrawRadiusPx, kHandleSegments);
    ++scene.handle_count;
  }
  AppendDisc(scene.handle_discs, geometry.rotate_handle_widget,
             CropGeometry::kCropRotateHandleDrawRadiusPx, kHandleSegments);
  ++scene.handle_count;

  return scene;
}

struct EditorOverlayItem::OverlayRootNode : public QSGNode {
  QSGGeometryNode* mask_node = nullptr;
  QSGGeometryNode* border_outer_node = nullptr;
  QSGGeometryNode* border_inner_node = nullptr;
  QSGGeometryNode* grip_outer_node = nullptr;
  QSGGeometryNode* grip_inner_node = nullptr;
  QSGGeometryNode* grid_node = nullptr;
  QSGGeometryNode* stem_outer_node = nullptr;
  QSGGeometryNode* stem_inner_node = nullptr;
  QSGGeometryNode* handle_node = nullptr;
  QSGGeometryNode* detail_roi_node = nullptr;
};

EditorOverlayItem::EditorOverlayItem(QQuickItem* parent) : QQuickItem(parent) {
  setFlag(ItemHasContents, true);
  setAcceptedMouseButtons(Qt::NoButton);
  setAcceptHoverEvents(false);
}

void EditorOverlayItem::setInteraction(EditorInteractionController* controller) {
  if (interaction_ == controller) {
    return;
  }
  bindInteraction(controller);
  emit InteractionChanged();
  refreshFromInteraction();
}

auto EditorOverlayItem::cropVisible() const -> bool { return last_scene_geometry_.has_crop; }

void EditorOverlayItem::refreshFromInteraction() {
  rebuildSceneGeometry();
  ++geometry_revision_;
  geometry_dirty_ = true;
  emit GeometryRevisionChanged();
  update();
}

void EditorOverlayItem::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
  QQuickItem::geometryChange(newGeometry, oldGeometry);
  if (newGeometry.size() != oldGeometry.size()) {
    refreshFromInteraction();
  }
}

void EditorOverlayItem::bindInteraction(EditorInteractionController* controller) {
  if (interaction_) {
    disconnect(interaction_, nullptr, this, nullptr);
  }
  interaction_ = controller;
  if (!interaction_) {
    return;
  }
  connect(interaction_, &EditorInteractionController::overlayGeometryChanged, this,
          &EditorOverlayItem::onInteractionOverlayChanged);
  connect(interaction_, &EditorInteractionController::cropChanged, this,
          &EditorOverlayItem::onInteractionOverlayChanged);
  connect(interaction_, &EditorInteractionController::viewChanged, this,
          &EditorOverlayItem::onInteractionOverlayChanged);
  connect(interaction_, &EditorInteractionController::viewportMetricsChanged, this,
          &EditorOverlayItem::onInteractionOverlayChanged);
  connect(interaction_, &EditorInteractionController::imageGeometryChanged, this,
          &EditorOverlayItem::onInteractionOverlayChanged);
}

void EditorOverlayItem::onInteractionOverlayChanged() { refreshFromInteraction(); }

void EditorOverlayItem::rebuildSceneGeometry() {
  if (!interaction_) {
    last_scene_geometry_ = {};
    return;
  }
  // Match legacy paint: draw crop chrome whenever overlay_visible is true.
  // tool_enabled only gates hit-testing, not drawing.
  last_scene_geometry_ = BuildOverlaySceneGeometry(interaction_->overlayGeometry(),
                                                   interaction_->cropOverlayVisible());
}

auto EditorOverlayItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData*) -> QSGNode* {
  auto* root = static_cast<OverlayRootNode*>(old_node);
  if (!root) {
    root = new OverlayRootNode;
  }

  if (!geometry_dirty_ && old_node) {
    return root;
  }
  geometry_dirty_ = false;

  auto replace_child = [root](QSGGeometryNode*& slot, QSGGeometryNode* next) {
    if (slot) {
      root->removeChildNode(slot);
      delete slot;
      slot = nullptr;
    }
    if (next) {
      root->appendChildNode(next);
      slot = next;
    }
  };

  const auto& scene = last_scene_geometry_;

  replace_child(root->mask_node,
                MakeColoredNode(scene.mask_triangles, QColor(0, 0, 0, 112),
                                QSGGeometry::DrawTriangles));

  replace_child(root->border_outer_node,
                MakeColoredNode(scene.border_lines, QColor(255, 255, 255, 235),
                                QSGGeometry::DrawLines, 3.0f));
  replace_child(root->border_inner_node,
                MakeColoredNode(scene.border_lines, QColor(22, 22, 22, 230),
                                QSGGeometry::DrawLines, 1.2f));

  replace_child(root->grip_outer_node,
                MakeColoredNode(scene.edge_grip_lines, QColor(255, 255, 255, 245),
                                QSGGeometry::DrawLines, 5.0f));
  replace_child(root->grip_inner_node,
                MakeColoredNode(scene.edge_grip_lines, QColor(28, 28, 28, 235),
                                QSGGeometry::DrawLines, 2.4f));

  replace_child(root->grid_node,
                MakeColoredNode(scene.grid_lines, QColor(255, 255, 255, 135),
                                QSGGeometry::DrawLines, 1.0f));

  replace_child(root->stem_outer_node,
                MakeColoredNode(scene.rotate_stem_line, QColor(255, 255, 255, 220),
                                QSGGeometry::DrawLines, 2.4f));
  replace_child(root->stem_inner_node,
                MakeColoredNode(scene.rotate_stem_line, QColor(25, 25, 25, 220),
                                QSGGeometry::DrawLines, 1.1f));

  const auto handle_tris = ExpandHandleDiscs(scene);
  replace_child(root->handle_node,
                MakeColoredNode(handle_tris, QColor(255, 255, 255, 245),
                                QSGGeometry::DrawTriangles));

  replace_child(root->detail_roi_node,
                MakeColoredNode(scene.detail_roi_lines, QColor(120, 200, 255, 200),
                                QSGGeometry::DrawLines, 1.5f));

  (void)kMaskAlpha;
  return root;
}

}  // namespace alcedo::editor_rhi
