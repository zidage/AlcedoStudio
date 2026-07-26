//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_tone_curve_item.hpp"

#include <QCursor>
#include <QMouseEvent>
#include <QSGFlatColorMaterial>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGNode>

#include <algorithm>
#include <cmath>

#include "ui/alcedo_main/album_backend/editor_tone_curve_model.hpp"
#include "ui/alcedo_main/editor_dialog/modules/curve.hpp"

namespace alcedo::ui {
namespace {

// Tight plot insets — the previous 22/12/14/24 left too much dead chrome and
// made the interactive area feel cramped in the side panel.
constexpr qreal kPlotLeft   = 8.0;
constexpr qreal kPlotTop    = 8.0;
constexpr qreal kPlotRight  = 8.0;
constexpr qreal kPlotBottom = 8.0;
constexpr int   kHandleFan  = 20;
constexpr qreal kPi         = 3.14159265358979323846;
// GL lineWidth is ignored on modern core profiles; draw the curve as a solid
// triangle ribbon instead (half-width in item px).
constexpr qreal kCurveHalfWidth = 1.75;
constexpr qreal kHitRadius      = 14.0;
// Pointer gain < 1: mouse motion is decelerated for finer curve control.
// Full plot drag maps to this fraction of normalized [0,1] (≈3× slower than 1:1).
constexpr qreal kPointerGain = 0.32;

void FillSolidGeometry(QSGGeometry* geometry, const std::vector<QPointF>& points,
                       QSGGeometry::DrawingMode mode, qreal line_width) {
  geometry->allocate(static_cast<int>(points.size()));
  geometry->setDrawingMode(mode);
  geometry->setLineWidth(static_cast<float>(line_width));
  auto* vertices = geometry->vertexDataAsPoint2D();
  for (size_t i = 0; i < points.size(); ++i) {
    vertices[i].set(static_cast<float>(points[i].x()), static_cast<float>(points[i].y()));
  }
  geometry->markVertexDataDirty();
}

void AppendDisc(std::vector<QPointF>& tris, const QPointF& center, qreal radius) {
  for (int i = 0; i < kHandleFan; ++i) {
    const qreal a0 = (2.0 * kPi * static_cast<qreal>(i)) / kHandleFan;
    const qreal a1 = (2.0 * kPi * static_cast<qreal>(i + 1)) / kHandleFan;
    tris.push_back(center);
    tris.emplace_back(center.x() + std::cos(a0) * radius, center.y() + std::sin(a0) * radius);
    tris.emplace_back(center.x() + std::cos(a1) * radius, center.y() + std::sin(a1) * radius);
  }
}

void AppendRect(std::vector<QPointF>& tris, const QRectF& r) {
  const QPointF tl = r.topLeft();
  const QPointF tr = r.topRight();
  const QPointF br = r.bottomRight();
  const QPointF bl = r.bottomLeft();
  tris.push_back(tl);
  tris.push_back(tr);
  tris.push_back(br);
  tris.push_back(tl);
  tris.push_back(br);
  tris.push_back(bl);
}

void AppendStrokeRibbon(std::vector<QPointF>& tris, const std::vector<QPointF>& samples,
                        qreal half_width) {
  if (samples.size() < 2 || half_width <= 0.0) {
    return;
  }
  for (size_t i = 0; i + 1 < samples.size(); ++i) {
    const QPointF a = samples[i];
    const QPointF b = samples[i + 1];
    const qreal   dx = b.x() - a.x();
    const qreal   dy = b.y() - a.y();
    const qreal   len = std::hypot(dx, dy);
    if (len < 1.0e-6) {
      continue;
    }
    const QPointF n(-dy / len * half_width, dx / len * half_width);
    const QPointF a0 = a + n;
    const QPointF a1 = a - n;
    const QPointF b0 = b + n;
    const QPointF b1 = b - n;
    tris.push_back(a0);
    tris.push_back(b0);
    tris.push_back(b1);
    tris.push_back(a0);
    tris.push_back(b1);
    tris.push_back(a1);
  }
}

void UpsertSolidNode(QSGNode* root, QSGGeometryNode*& slot, const std::vector<QPointF>& points,
                     const QColor& color, QSGGeometry::DrawingMode mode, qreal line_width = 1.0) {
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
    slot           = new QSGGeometryNode;
    auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(),
                                     static_cast<int>(points.size()));
    slot->setGeometry(geometry);
    slot->setFlag(QSGNode::OwnsGeometry);
    auto* material = new QSGFlatColorMaterial;
    slot->setMaterial(material);
    slot->setFlag(QSGNode::OwnsMaterial);
    root->appendChildNode(slot);
  }

  FillSolidGeometry(slot->geometry(), points, mode, line_width);
  if (auto* mat = static_cast<QSGFlatColorMaterial*>(slot->material())) {
    mat->setColor(color);
  }
  slot->markDirty(QSGNode::DirtyGeometry | QSGNode::DirtyMaterial);
}

}  // namespace

auto ToneCurvePlotRect(qreal width, qreal height) -> QRectF {
  return QRectF(kPlotLeft, kPlotTop, std::max(30.0, width - kPlotLeft - kPlotRight),
                std::max(30.0, height - kPlotTop - kPlotBottom));
}

auto ToneCurveToWidgetPoint(const QPointF& normalized, const QRectF& plot) -> QPointF {
  const qreal x = plot.left() + normalized.x() * plot.width();
  const qreal y = plot.bottom() - normalized.y() * plot.height();
  return QPointF(x, y);
}

auto ToneCurveToNormalizedPoint(const QPointF& widget_point, const QRectF& plot) -> QPointF {
  const qreal nx =
      std::clamp((widget_point.x() - plot.left()) / std::max(1.0, plot.width()), 0.0, 1.0);
  const qreal ny =
      std::clamp((plot.bottom() - widget_point.y()) / std::max(1.0, plot.height()), 0.0, 1.0);
  return QPointF(nx, ny);
}

auto ToneCurveHitTestPoint(const QPointF& widget_point, const std::vector<QPointF>& normalized_points,
                           const QRectF& plot, qreal hit_radius) -> int {
  const qreal hit_radius_sq = hit_radius * hit_radius;
  for (int i = static_cast<int>(normalized_points.size()) - 1; i >= 0; --i) {
    const QPointF p  = ToneCurveToWidgetPoint(normalized_points[static_cast<size_t>(i)], plot);
    const qreal   dx = p.x() - widget_point.x();
    const qreal   dy = p.y() - widget_point.y();
    if ((dx * dx + dy * dy) <= hit_radius_sq) {
      return i;
    }
  }
  return -1;
}

auto BuildToneCurveSceneGeometry(const std::vector<QPointF>& normalized_points, qreal item_width,
                                 qreal item_height, int active_index, int sample_count)
    -> ToneCurveSceneGeometry {
  ToneCurveSceneGeometry g;
  g.plot_rect    = ToneCurvePlotRect(item_width, item_height);
  g.active_index = active_index;
  g.sample_count = std::max(2, sample_count);

  for (int i = 1; i < 4; ++i) {
    const qreal t  = static_cast<qreal>(i) / 4.0;
    const qreal gx = g.plot_rect.left() + t * g.plot_rect.width();
    const qreal gy = g.plot_rect.top() + t * g.plot_rect.height();
    g.grid_segments.emplace_back(gx, g.plot_rect.top());
    g.grid_segments.emplace_back(gx, g.plot_rect.bottom());
    g.grid_segments.emplace_back(g.plot_rect.left(), gy);
    g.grid_segments.emplace_back(g.plot_rect.right(), gy);
  }

  g.diagonal = {QPointF(g.plot_rect.left(), g.plot_rect.bottom()),
                QPointF(g.plot_rect.right(), g.plot_rect.top())};

  const auto points =
      normalized_points.empty() ? curve::DefaultCurveControlPoints() : normalized_points;
  const auto cache = curve::BuildCurveHermiteCache(points);
  g.curve_samples.reserve(static_cast<size_t>(g.sample_count) + 1);
  for (int i = 0; i <= g.sample_count; ++i) {
    const float x = static_cast<float>(i) / static_cast<float>(g.sample_count);
    const float y = curve::EvaluateCurveHermite(x, points, cache);
    g.curve_samples.push_back(ToneCurveToWidgetPoint(QPointF(x, y), g.plot_rect));
  }

  g.handles.reserve(points.size());
  for (const auto& p : points) {
    g.handles.push_back(ToneCurveToWidgetPoint(p, g.plot_rect));
  }
  return g;
}

struct EditorToneCurveItem::CurveRootNode : public QSGNode {
  QSGGeometryNode* background = nullptr;
  QSGGeometryNode* plot       = nullptr;
  QSGGeometryNode* grid       = nullptr;
  QSGGeometryNode* diagonal   = nullptr;
  QSGGeometryNode* curve      = nullptr;
  QSGGeometryNode* outlines   = nullptr;
  QSGGeometryNode* handles    = nullptr;
  QSGGeometryNode* active     = nullptr;
};

EditorToneCurveItem::EditorToneCurveItem(QQuickItem* parent) : QQuickItem(parent) {
  setFlag(ItemHasContents, true);
  setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);
  setAcceptHoverEvents(true);
  setCursor(QCursor(Qt::CrossCursor));
  setImplicitHeight(280);
  setImplicitWidth(200);
  setClip(true);
}

void EditorToneCurveItem::setModel(EditorToneCurveModel* model) {
  if (model_ == model) {
    return;
  }
  bindModel(model);
  emit modelChanged();
  rebuildGeometry();
}

void EditorToneCurveItem::setBackgroundColor(const QColor& c) {
  if (background_color_ == c) {
    return;
  }
  background_color_ = c;
  emit colorsChanged();
  markGeometryDirty();
}

void EditorToneCurveItem::setPlotColor(const QColor& c) {
  if (plot_color_ == c) {
    return;
  }
  plot_color_ = c;
  emit colorsChanged();
  markGeometryDirty();
}

void EditorToneCurveItem::setGridColor(const QColor& c) {
  if (grid_color_ == c) {
    return;
  }
  grid_color_ = c;
  emit colorsChanged();
  markGeometryDirty();
}

void EditorToneCurveItem::setDiagonalColor(const QColor& c) {
  if (diagonal_color_ == c) {
    return;
  }
  diagonal_color_ = c;
  emit colorsChanged();
  markGeometryDirty();
}

void EditorToneCurveItem::setCurveColor(const QColor& c) {
  if (curve_color_ == c) {
    return;
  }
  curve_color_ = c;
  emit colorsChanged();
  markGeometryDirty();
}

void EditorToneCurveItem::setHandleColor(const QColor& c) {
  if (handle_color_ == c) {
    return;
  }
  handle_color_ = c;
  emit colorsChanged();
  markGeometryDirty();
}

void EditorToneCurveItem::setHandleActiveColor(const QColor& c) {
  if (handle_active_color_ == c) {
    return;
  }
  handle_active_color_ = c;
  emit colorsChanged();
  markGeometryDirty();
}

void EditorToneCurveItem::setHandleOutlineColor(const QColor& c) {
  if (handle_outline_color_ == c) {
    return;
  }
  handle_outline_color_ = c;
  emit colorsChanged();
  markGeometryDirty();
}

void EditorToneCurveItem::refreshGeometry() { rebuildGeometry(); }

void EditorToneCurveItem::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
  QQuickItem::geometryChange(newGeometry, oldGeometry);
  if (newGeometry.size() != oldGeometry.size()) {
    rebuildGeometry();
  }
}

void EditorToneCurveItem::bindModel(EditorToneCurveModel* model) {
  if (model_) {
    disconnect(model_, nullptr, this, nullptr);
  }
  model_ = model;
  if (!model_) {
    return;
  }
  connect(model_, &EditorToneCurveModel::pointsChanged, this,
          &EditorToneCurveItem::onModelPointsChanged);
  connect(model_, &EditorToneCurveModel::activeIndexChanged, this,
          &EditorToneCurveItem::onModelPointsChanged);
  connect(model_, &EditorToneCurveModel::enabledChanged, this, [this]() {
    setAcceptedMouseButtons(model_ && model_->enabled() ? (Qt::LeftButton | Qt::RightButton)
                                                        : Qt::MouseButtons{});
  });
}

void EditorToneCurveItem::onModelPointsChanged() { rebuildGeometry(); }

void EditorToneCurveItem::rebuildGeometry() {
  const auto& points =
      model_ ? model_->controlPoints() : curve::DefaultCurveControlPoints();
  const int active = model_ ? model_->activeIndex() : -1;
  last_geometry_ =
      BuildToneCurveSceneGeometry(points, width(), height(), active, /*sample_count=*/240);
  ++geometry_revision_;
  geometry_dirty_ = true;
  emit geometryRevisionChanged();
  update();
}

void EditorToneCurveItem::markGeometryDirty() {
  geometry_dirty_ = true;
  update();
}

auto EditorToneCurveItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData*) -> QSGNode* {
  auto* root = static_cast<CurveRootNode*>(old_node);
  if (!root) {
    root = new CurveRootNode;
  }

  if (!geometry_dirty_ && old_node) {
    return root;
  }
  geometry_dirty_ = false;

  std::vector<QPointF> bg_tris;
  AppendRect(bg_tris, QRectF(0, 0, width(), height()));
  UpsertSolidNode(root, root->background, bg_tris, background_color_,
                  QSGGeometry::DrawTriangles);

  std::vector<QPointF> plot_tris;
  AppendRect(plot_tris, last_geometry_.plot_rect);
  UpsertSolidNode(root, root->plot, plot_tris, plot_color_, QSGGeometry::DrawTriangles);

  UpsertSolidNode(root, root->grid, last_geometry_.grid_segments, grid_color_,
                  QSGGeometry::DrawLines, 1.0);
  UpsertSolidNode(root, root->diagonal, last_geometry_.diagonal, diagonal_color_,
                  QSGGeometry::DrawLines, 1.0);

  std::vector<QPointF> curve_tris;
  AppendStrokeRibbon(curve_tris, last_geometry_.curve_samples, kCurveHalfWidth);
  UpsertSolidNode(root, root->curve, curve_tris, curve_color_, QSGGeometry::DrawTriangles);

  std::vector<QPointF> outline_tris;
  std::vector<QPointF> handle_tris;
  std::vector<QPointF> active_tris;
  for (size_t i = 0; i < last_geometry_.handles.size(); ++i) {
    const bool  active = static_cast<int>(i) == last_geometry_.active_index;
    const qreal radius = active ? 6.5 : 5.5;
    AppendDisc(outline_tris, last_geometry_.handles[i], radius + 1.5);
    if (active) {
      AppendDisc(active_tris, last_geometry_.handles[i], radius);
    } else {
      AppendDisc(handle_tris, last_geometry_.handles[i], radius);
    }
  }
  UpsertSolidNode(root, root->outlines, outline_tris, handle_outline_color_,
                  QSGGeometry::DrawTriangles);
  UpsertSolidNode(root, root->handles, handle_tris, handle_color_, QSGGeometry::DrawTriangles);
  UpsertSolidNode(root, root->active, active_tris, handle_active_color_,
                  QSGGeometry::DrawTriangles);

  return root;
}

void EditorToneCurveItem::mousePressEvent(QMouseEvent* event) {
  if (!event || !model_ || !model_->enabled() || !isEnabled()) {
    QQuickItem::mousePressEvent(event);
    return;
  }

  const QPointF pos = event->position();
  if (event->button() == Qt::RightButton) {
    const int hit = ToneCurveHitTestPoint(pos, model_->controlPoints(), last_geometry_.plot_rect,
                                          kHitRadius);
    if (hit > 0 && hit + 1 < model_->pointCount()) {
      model_->removePoint(hit);
      dragging_ = false;
      event->accept();
      return;
    }
    event->accept();
    return;
  }

  if (event->button() != Qt::LeftButton) {
    QQuickItem::mousePressEvent(event);
    return;
  }

  const int hit = ToneCurveHitTestPoint(pos, model_->controlPoints(), last_geometry_.plot_rect,
                                        kHitRadius);
  if (hit >= 0) {
    model_->beginDrag(hit);
    dragging_                 = true;
    drag_origin_widget_       = pos;
    drag_origin_normalized_   = model_->controlPoints()[static_cast<size_t>(hit)];
    event->accept();
    return;
  }

  if (!last_geometry_.plot_rect.contains(pos)) {
    event->accept();
    return;
  }

  const QPointF normalized = ToneCurveToNormalizedPoint(pos, last_geometry_.plot_rect);
  const int     inserted   = model_->insertPoint(normalized.x(), normalized.y());
  dragging_                = inserted >= 0;
  if (dragging_) {
    drag_origin_widget_     = pos;
    drag_origin_normalized_ = model_->controlPoints()[static_cast<size_t>(inserted)];
  }
  event->accept();
}

void EditorToneCurveItem::mouseMoveEvent(QMouseEvent* event) {
  if (!event || !dragging_ || !model_ || !model_->dragActive()) {
    QQuickItem::mouseMoveEvent(event);
    return;
  }
  // Relative drag with pointer gain: pointer motion is decelerated so fine
  // curve shaping does not require microscopic hand movements.
  const QPointF abs_norm = ToneCurveToNormalizedPoint(event->position(), last_geometry_.plot_rect);
  const QPointF press_norm =
      ToneCurveToNormalizedPoint(drag_origin_widget_, last_geometry_.plot_rect);
  const QPointF delta = abs_norm - press_norm;
  const QPointF target(drag_origin_normalized_.x() + delta.x() * kPointerGain,
                       drag_origin_normalized_.y() + delta.y() * kPointerGain);
  model_->updateDrag(target.x(), target.y());
  event->accept();
}

void EditorToneCurveItem::mouseReleaseEvent(QMouseEvent* event) {
  if (!event || event->button() != Qt::LeftButton) {
    QQuickItem::mouseReleaseEvent(event);
    return;
  }
  if (dragging_ && model_ && model_->dragActive()) {
    model_->finishDrag();
  }
  dragging_ = false;
  event->accept();
}

void EditorToneCurveItem::mouseDoubleClickEvent(QMouseEvent* event) {
  if (!event || !model_ || !model_->enabled()) {
    QQuickItem::mouseDoubleClickEvent(event);
    return;
  }
  if (event->button() == Qt::LeftButton) {
    model_->reset();
    dragging_ = false;
    event->accept();
    return;
  }
  QQuickItem::mouseDoubleClickEvent(event);
}

}  // namespace alcedo::ui
