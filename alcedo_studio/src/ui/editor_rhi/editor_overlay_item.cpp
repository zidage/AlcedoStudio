//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/editor_rhi/editor_overlay_item.hpp"

#include "ui/editor_rhi/editor_interaction_controller.hpp"

#include <QMetaObject>
#include <QSGVertexColorMaterial>

#include <algorithm>
#include <cmath>
#include <utility>

#include "ui/edit_viewer/crop_geometry.hpp"

namespace alcedo::editor_rhi {
namespace {

constexpr int kCapSegments = 10;
constexpr float kMaskAlpha = 112.0f / 255.0f;

auto Cross(const QPointF& o, const QPointF& a, const QPointF& b) -> double {
  return (a.x() - o.x()) * (b.y() - o.y()) - (a.y() - o.y()) * (b.x() - o.x());
}

auto PolygonArea(const std::vector<QPointF>& poly) -> double {
  if (poly.size() < 3) {
    return 0.0;
  }
  double area = 0.0;
  for (size_t i = 0; i < poly.size(); ++i) {
    const QPointF& a = poly[i];
    const QPointF& b = poly[(i + 1) % poly.size()];
    area += (a.x() * b.y()) - (b.x() * a.y());
  }
  return area * 0.5;
}

// Clip a convex polygon to the half-plane where Cross(a, b, p) has the requested sign.
// keep_positive=true keeps Cross(a,b,p) >= 0 (left of directed edge a→b).
auto ClipPolygonToHalfPlane(const std::vector<QPointF>& input, const QPointF& a, const QPointF& b,
                            bool keep_positive) -> std::vector<QPointF> {
  if (input.empty()) {
    return {};
  }
  auto inside = [&](const QPointF& p) {
    const double c = Cross(a, b, p);
    return keep_positive ? (c >= -1e-9) : (c <= 1e-9);
  };
  auto intersect = [&](const QPointF& p, const QPointF& q) -> QPointF {
    const double c1 = Cross(a, b, p);
    const double c2 = Cross(a, b, q);
    const double t = c1 / (c1 - c2 + 1e-30);
    return {p.x() + t * (q.x() - p.x()), p.y() + t * (q.y() - p.y())};
  };

  std::vector<QPointF> output;
  output.reserve(input.size() + 1);
  for (size_t i = 0; i < input.size(); ++i) {
    const QPointF& cur = input[i];
    const QPointF& prev = input[(i + input.size() - 1) % input.size()];
    const bool cur_in = inside(cur);
    const bool prev_in = inside(prev);
    if (cur_in) {
      if (!prev_in) {
        output.push_back(intersect(prev, cur));
      }
      output.push_back(cur);
    } else if (prev_in) {
      output.push_back(intersect(prev, cur));
    }
  }
  return output;
}

void AppendConvexPolygonTriangles(std::vector<QPointF>& triangles,
                                  const std::vector<QPointF>& poly) {
  if (poly.size() < 3) {
    return;
  }
  // Convex poly → safe fan from vertex 0.
  for (size_t i = 1; i + 1 < poly.size(); ++i) {
    triangles.push_back(poly[0]);
    triangles.push_back(poly[i]);
    triangles.push_back(poly[i + 1]);
  }
}

// Dim = image \ crop for a convex crop. Since crop is the intersection of four
// interior half-planes, its exterior is the union of the four exterior half-planes:
//   image \ crop = ∪_edge (image ∩ exterior(edge))
// Overdraw in exterior corner wedges is fine (same solid color).
void AppendDimMaskWithHole(std::vector<QPointF>& triangles, const QRectF& image,
                           const std::array<QPointF, 4>& crop_corners) {
  std::vector<QPointF> image_poly = {image.topLeft(), image.topRight(), image.bottomRight(),
                                     image.bottomLeft()};
  // Ensure CCW so interior is left of edges.
  if (PolygonArea(image_poly) < 0.0) {
    std::reverse(image_poly.begin(), image_poly.end());
  }

  // Determine crop winding: interior of crop is left of directed edges for CCW.
  std::array<QPointF, 4> ordered = crop_corners;
  double crop_area = 0.0;
  for (size_t i = 0; i < 4; ++i) {
    const QPointF& a = ordered[i];
    const QPointF& b = ordered[(i + 1) % 4];
    crop_area += (a.x() * b.y()) - (b.x() * a.y());
  }
  if (crop_area < 0.0) {
    std::reverse(ordered.begin(), ordered.end());
  }

  for (size_t i = 0; i < 4; ++i) {
    const QPointF& a = ordered[i];
    const QPointF& b = ordered[(i + 1) % 4];
    // Exterior of CCW crop edge a→b is Cross(a,b,p) < 0 (right of the edge).
    auto clipped = ClipPolygonToHalfPlane(image_poly, a, b, /*keep_positive=*/false);
    AppendConvexPolygonTriangles(triangles, clipped);
  }
}

auto PerpUnit(const QPointF& a, const QPointF& b) -> QPointF {
  const double dx = b.x() - a.x();
  const double dy = b.y() - a.y();
  const double len = std::hypot(dx, dy);
  if (len < 1e-9) {
    return {1.0, 0.0};
  }
  return {-dy / len, dx / len};
}

void AppendThickLine(std::vector<QPointF>& tris, const QPointF& a, const QPointF& b, float width) {
  const QPointF n = PerpUnit(a, b);
  const double half = static_cast<double>(width) * 0.5;
  const QPointF o = {n.x() * half, n.y() * half};
  const QPointF p0 = a + o;
  const QPointF p1 = a - o;
  const QPointF p2 = b - o;
  const QPointF p3 = b + o;
  tris.push_back(p0);
  tris.push_back(p1);
  tris.push_back(p2);
  tris.push_back(p0);
  tris.push_back(p2);
  tris.push_back(p3);
}

void AppendRoundCap(std::vector<QPointF>& tris, const QPointF& center, const QPointF& along,
                    float radius, int segments) {
  const double len = std::hypot(along.x(), along.y());
  if (len < 1e-9 || radius <= 0.0f || segments < 2) {
    return;
  }
  const QPointF dir = {along.x() / len, along.y() / len};
  const QPointF n = {-dir.y(), dir.x()};
  // Semicircle on the outward side of the endpoint.
  const double start = std::atan2(n.y(), n.x());
  for (int i = 0; i < segments; ++i) {
    const double a0 = start + (3.14159265358979323846 * static_cast<double>(i) / segments);
    const double a1 = start + (3.14159265358979323846 * static_cast<double>(i + 1) / segments);
    tris.push_back(center);
    tris.push_back(QPointF(center.x() + std::cos(a0) * radius, center.y() + std::sin(a0) * radius));
    tris.push_back(QPointF(center.x() + std::cos(a1) * radius, center.y() + std::sin(a1) * radius));
  }
}

void AppendStrokeSegment(std::vector<QPointF>& tris, const QPointF& a, const QPointF& b, float width,
                         bool round_caps) {
  AppendThickLine(tris, a, b, width);
  if (round_caps) {
    const QPointF ab = b - a;
    AppendRoundCap(tris, a, a - b, width * 0.5f, kCapSegments);
    AppendRoundCap(tris, b, ab, width * 0.5f, kCapSegments);
  }
}

void AppendPolygonStroke(std::vector<QPointF>& tris, const std::array<QPointF, 4>& corners,
                         float width, bool closed) {
  const size_t n = corners.size();
  const size_t count = closed ? n : n - 1;
  for (size_t i = 0; i < count; ++i) {
    AppendStrokeSegment(tris, corners[i], corners[(i + 1) % n], width, /*round_caps=*/false);
  }
}

void AppendDashedStroke(std::vector<QPointF>& tris, const QPointF& a, const QPointF& b, float width,
                        float dash_len, float gap_len) {
  const double dx = b.x() - a.x();
  const double dy = b.y() - a.y();
  const double len = std::hypot(dx, dy);
  if (len < 1e-6) {
    return;
  }
  const double ux = dx / len;
  const double uy = dy / len;
  double t = 0.0;
  bool draw = true;
  while (t < len - 1e-6) {
    const double remaining = len - t;
    const double step = std::min(remaining, static_cast<double>(draw ? dash_len : gap_len));
    if (draw && step > 1e-6) {
      const QPointF p0(a.x() + ux * t, a.y() + uy * t);
      const QPointF p1(a.x() + ux * (t + step), a.y() + uy * (t + step));
      AppendThickLine(tris, p0, p1, width);
    }
    t += step;
    draw = !draw;
  }
}

void AppendFilledDisc(std::vector<QPointF>& tris, const QPointF& center, float radius, int segments) {
  if (radius <= 0.0f || segments < 3) {
    return;
  }
  for (int i = 0; i < segments; ++i) {
    const float a0 = (static_cast<float>(i) / static_cast<float>(segments)) * 6.28318530718f;
    const float a1 = (static_cast<float>(i + 1) / static_cast<float>(segments)) * 6.28318530718f;
    tris.push_back(center);
    tris.push_back(QPointF(center.x() + std::cos(a0) * radius, center.y() + std::sin(a0) * radius));
    tris.push_back(QPointF(center.x() + std::cos(a1) * radius, center.y() + std::sin(a1) * radius));
  }
}

void AppendRing(std::vector<QPointF>& tris, const QPointF& center, float inner_r, float outer_r,
                int segments) {
  if (outer_r <= inner_r || segments < 3) {
    return;
  }
  for (int i = 0; i < segments; ++i) {
    const float a0 = (static_cast<float>(i) / static_cast<float>(segments)) * 6.28318530718f;
    const float a1 = (static_cast<float>(i + 1) / static_cast<float>(segments)) * 6.28318530718f;
    const QPointF o0(center.x() + std::cos(a0) * outer_r, center.y() + std::sin(a0) * outer_r);
    const QPointF o1(center.x() + std::cos(a1) * outer_r, center.y() + std::sin(a1) * outer_r);
    const QPointF i0(center.x() + std::cos(a0) * inner_r, center.y() + std::sin(a0) * inner_r);
    const QPointF i1(center.x() + std::cos(a1) * inner_r, center.y() + std::sin(a1) * inner_r);
    tris.push_back(o0);
    tris.push_back(i0);
    tris.push_back(i1);
    tris.push_back(o0);
    tris.push_back(i1);
    tris.push_back(o1);
  }
}

void FillColoredGeometry(QSGGeometry* geometry, const std::vector<QPointF>& points,
                         const QColor& color) {
  geometry->allocate(static_cast<int>(points.size()));
  geometry->setDrawingMode(QSGGeometry::DrawTriangles);
  auto* vertices = geometry->vertexDataAsColoredPoint2D();
  const uchar r = static_cast<uchar>(color.red());
  const uchar g = static_cast<uchar>(color.green());
  const uchar b = static_cast<uchar>(color.blue());
  const uchar a = static_cast<uchar>(color.alpha());
  for (size_t i = 0; i < points.size(); ++i) {
    vertices[i].set(static_cast<float>(points[i].x()), static_cast<float>(points[i].y()), r, g, b,
                    a);
  }
  geometry->markVertexDataDirty();
}

// Update an existing node in place, or create once. Never deletes material.
void UpsertTriangleNode(QSGNode* root, QSGGeometryNode*& slot, const std::vector<QPointF>& points,
                        const QColor& color, int& create_count) {
  if (points.empty()) {
    if (slot) {
      slot->setFlag(QSGNode::OwnedByParent, false);
      root->removeChildNode(slot);
      delete slot;
      slot = nullptr;
    }
    return;
  }

  if (!slot) {
    slot = new QSGGeometryNode;
    auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(),
                                     static_cast<int>(points.size()));
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    slot->setGeometry(geometry);
    slot->setFlag(QSGNode::OwnsGeometry);
    auto* material = new QSGVertexColorMaterial;
    slot->setMaterial(material);
    slot->setFlag(QSGNode::OwnsMaterial);
    root->appendChildNode(slot);
    ++create_count;
  }

  FillColoredGeometry(slot->geometry(), points, color);
  slot->markDirty(QSGNode::DirtyGeometry);
}

}  // namespace

auto BuildOverlaySceneGeometry(const CropOverlayWidgetGeometry& geometry, bool crop_tool_visible)
    -> OverlaySceneGeometry {
  OverlaySceneGeometry scene;

  if (geometry.detail_roi_valid) {
    scene.has_detail_roi = true;
    AppendPolygonStroke(scene.detail_roi_triangles, geometry.detail_roi_corners_widget, 1.5f,
                        /*closed=*/true);
  }

  if (!crop_tool_visible || !geometry.image_rect_valid || !geometry.crop_corners_valid) {
    return scene;
  }
  scene.has_crop = true;

  // Dim mask: hole-safe ear-clip of path-subtraction polygons (never fan a hole).
  AppendDimMaskWithHole(scene.mask_triangles, geometry.image_rect, geometry.crop_corners_widget);

  // Dual border strokes as triangles (legacy white 3.0 / dark 1.2).
  AppendPolygonStroke(scene.border_outer_triangles, geometry.crop_corners_widget, 3.0f, true);
  AppendPolygonStroke(scene.border_inner_triangles, geometry.crop_corners_widget, 1.2f, true);

  auto append_edge_grip = [&](const QPointF& a, const QPointF& b) {
    const QPointF grip_a = CropGeometry::LerpPoint(a, b, 0.38f);
    const QPointF grip_b = CropGeometry::LerpPoint(a, b, 0.62f);
    AppendStrokeSegment(scene.grip_outer_triangles, grip_a, grip_b, 5.0f, true);
    AppendStrokeSegment(scene.grip_inner_triangles, grip_a, grip_b, 2.4f, true);
  };
  append_edge_grip(geometry.crop_corners_widget[0], geometry.crop_corners_widget[1]);
  append_edge_grip(geometry.crop_corners_widget[1], geometry.crop_corners_widget[2]);
  append_edge_grip(geometry.crop_corners_widget[2], geometry.crop_corners_widget[3]);
  append_edge_grip(geometry.crop_corners_widget[3], geometry.crop_corners_widget[0]);

  // Dashed thirds (legacy Qt::DashLine).
  constexpr float kDash = 6.0f;
  constexpr float kGap = 4.0f;
  for (const float t : {1.0f / 3.0f, 2.0f / 3.0f}) {
    AppendDashedStroke(
        scene.grid_triangles,
        CropGeometry::LerpPoint(geometry.crop_corners_widget[0], geometry.crop_corners_widget[1], t),
        CropGeometry::LerpPoint(geometry.crop_corners_widget[3], geometry.crop_corners_widget[2], t),
        1.0f, kDash, kGap);
    AppendDashedStroke(
        scene.grid_triangles,
        CropGeometry::LerpPoint(geometry.crop_corners_widget[0], geometry.crop_corners_widget[3], t),
        CropGeometry::LerpPoint(geometry.crop_corners_widget[1], geometry.crop_corners_widget[2], t),
        1.0f, kDash, kGap);
  }

  AppendStrokeSegment(scene.stem_outer_triangles, geometry.rotate_stem_widget,
                      geometry.rotate_handle_widget, 2.4f, true);
  AppendStrokeSegment(scene.stem_inner_triangles, geometry.rotate_stem_widget,
                      geometry.rotate_handle_widget, 1.1f, true);

  constexpr int kHandleSegs = 16;
  for (const auto& corner : geometry.crop_corners_widget) {
    const float r = CropGeometry::kCropCornerDrawRadiusPx;
    AppendRing(scene.handle_outline_triangles, corner, r, r + 1.2f, kHandleSegs);
    AppendFilledDisc(scene.handle_fill_triangles, corner, r, kHandleSegs);
    ++scene.handle_count;
  }
  {
    const float r = CropGeometry::kCropRotateHandleDrawRadiusPx;
    AppendRing(scene.handle_outline_triangles, geometry.rotate_handle_widget, r, r + 1.2f,
               kHandleSegs);
    AppendFilledDisc(scene.handle_fill_triangles, geometry.rotate_handle_widget, r, kHandleSegs);
    ++scene.handle_count;
  }

  (void)kMaskAlpha;
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
  QSGGeometryNode* handle_outline_node = nullptr;
  QSGGeometryNode* handle_fill_node = nullptr;
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
  scheduleRebuildFromInteraction();
}

auto EditorOverlayItem::cropVisible() const -> bool { return last_scene_geometry_.has_crop; }

void EditorOverlayItem::refreshFromInteraction() {
  rebuildSceneGeometry();
  ++geometry_revision_;
  ++geometry_rebuild_count_;
  geometry_dirty_ = true;
  rebuild_scheduled_ = false;
  emit GeometryRevisionChanged();
  update();
}

void EditorOverlayItem::scheduleRebuildFromInteraction() {
  if (rebuild_scheduled_) {
    return;
  }
  rebuild_scheduled_ = true;
  // Coalesce multi-signal bursts (view+crop+overlay) into one rebuild per event loop turn.
  QMetaObject::invokeMethod(
      this,
      [this]() {
        if (!rebuild_scheduled_) {
          return;
        }
        refreshFromInteraction();
      },
      Qt::QueuedConnection);
}

void EditorOverlayItem::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
  QQuickItem::geometryChange(newGeometry, oldGeometry);
  if (newGeometry.size() != oldGeometry.size()) {
    scheduleRebuildFromInteraction();
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
  // Single slot for all geometry-affecting signals — coalesced by scheduleRebuild.
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

void EditorOverlayItem::onInteractionOverlayChanged() { scheduleRebuildFromInteraction(); }

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
    ++paint_node_create_count_;
  }

  if (!geometry_dirty_ && old_node) {
    return root;
  }
  geometry_dirty_ = false;

  const auto& scene = last_scene_geometry_;
  int creates = 0;

  UpsertTriangleNode(root, root->mask_node, scene.mask_triangles, QColor(0, 0, 0, 112), creates);
  UpsertTriangleNode(root, root->border_outer_node, scene.border_outer_triangles,
                     QColor(255, 255, 255, 235), creates);
  UpsertTriangleNode(root, root->border_inner_node, scene.border_inner_triangles,
                     QColor(22, 22, 22, 230), creates);
  UpsertTriangleNode(root, root->grip_outer_node, scene.grip_outer_triangles,
                     QColor(255, 255, 255, 245), creates);
  UpsertTriangleNode(root, root->grip_inner_node, scene.grip_inner_triangles,
                     QColor(28, 28, 28, 235), creates);
  UpsertTriangleNode(root, root->grid_node, scene.grid_triangles, QColor(255, 255, 255, 135),
                     creates);
  UpsertTriangleNode(root, root->stem_outer_node, scene.stem_outer_triangles,
                     QColor(255, 255, 255, 220), creates);
  UpsertTriangleNode(root, root->stem_inner_node, scene.stem_inner_triangles,
                     QColor(25, 25, 25, 220), creates);
  UpsertTriangleNode(root, root->handle_outline_node, scene.handle_outline_triangles,
                     QColor(28, 28, 28, 230), creates);
  UpsertTriangleNode(root, root->handle_fill_node, scene.handle_fill_triangles,
                     QColor(255, 255, 255, 245), creates);
  UpsertTriangleNode(root, root->detail_roi_node, scene.detail_roi_triangles,
                     QColor(120, 200, 255, 200), creates);

  paint_node_create_count_ += creates;
  return root;
}

}  // namespace alcedo::editor_rhi
