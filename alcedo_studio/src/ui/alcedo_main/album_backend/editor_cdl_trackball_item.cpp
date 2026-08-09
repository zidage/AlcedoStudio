//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_cdl_trackball_item.hpp"

#include <QCursor>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QSGFlatColorMaterial>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGNode>
#include <QSGSimpleTextureNode>
#include <algorithm>
#include <cmath>
#include <numbers>

#include "ui/alcedo_main/editor_support/modules/color_wheel.hpp"

namespace alcedo::ui {
namespace {

constexpr qreal kDiscInset  = 3.0;
constexpr int   kHandleFan  = 48;
constexpr qreal kPi         = 3.14159265358979323846;
// Soft edge width in supersampled pixels for anti-aliased disc perimeter.
constexpr float kSoftEdgePx = 1.75f;

void            FillSolidGeometry(QSGGeometry* geometry, const std::vector<QPointF>& points,
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

void AppendRing(std::vector<QPointF>& segs, const QRectF& disc, int segments = 64) {
  const QPointF c = disc.center();
  const qreal   r = disc.width() * 0.5;
  for (int i = 0; i < segments; ++i) {
    const qreal a0 = (2.0 * kPi * static_cast<qreal>(i)) / segments;
    const qreal a1 = (2.0 * kPi * static_cast<qreal>(i + 1)) / segments;
    segs.emplace_back(c.x() + std::cos(a0) * r, c.y() + std::sin(a0) * r);
    segs.emplace_back(c.x() + std::cos(a1) * r, c.y() + std::sin(a1) * r);
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
    slot = new QSGGeometryNode;
    auto* geometry =
        new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), static_cast<int>(points.size()));
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

auto BuildWheelImage(int size) -> QImage {
  if (size <= 0) {
    return {};
  }
  QImage image(size, size, QImage::Format_ARGB32_Premultiplied);
  image.fill(Qt::transparent);
  const float radius = static_cast<float>(size) * 0.5f;
  const float cx     = radius - 0.5f;
  const float cy     = radius - 0.5f;
  // Soft edge in normalized units so the rim does not stair-step.
  const float soft   = std::clamp(kSoftEdgePx / std::max(radius, 1.0f), 0.002f, 0.08f);
  for (int y = 0; y < size; ++y) {
    auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
    for (int x = 0; x < size; ++x) {
      const float dx = (static_cast<float>(x) - cx) / std::max(radius, 1.0f);
      const float dy = (cy - static_cast<float>(y)) / std::max(radius, 1.0f);
      const float r  = std::sqrt(dx * dx + dy * dy);
      if (r >= 1.0f + soft) {
        row[x] = qRgba(0, 0, 0, 0);
        continue;
      }
      float alpha = 1.0f;
      if (r > 1.0f - soft) {
        // Linear falloff across the rim (premultiplied later).
        alpha = std::clamp((1.0f + soft - r) / (2.0f * soft), 0.0f, 1.0f);
      }
      float h = std::atan2(dy, dx) / (2.0f * std::numbers::pi_v<float>);
      if (h < 0.0f) {
        h += 1.0f;
      }
      const float  sat   = std::clamp(r, 0.0f, 1.0f);
      const QColor color = QColor::fromHsvF(h, sat, 1.0f);
      const int    a     = static_cast<int>(std::lround(alpha * 255.0f));
      const int    pr    = (color.red() * a + 127) / 255;
      const int    pg    = (color.green() * a + 127) / 255;
      const int    pb    = (color.blue() * a + 127) / 255;
      row[x]             = qRgba(pr, pg, pb, a);
    }
  }
  return image;
}

}  // namespace

auto CdlTrackballDiscRect(qreal width, qreal height) -> QRectF {
  const qreal side = std::max(1.0, std::min(width, height) - 2.0 * kDiscInset);
  const qreal left = (width - side) * 0.5;
  const qreal top  = (height - side) * 0.5;
  return QRectF(left, top, side, side);
}

auto CdlTrackballToWidgetPoint(const QPointF& normalized, const QRectF& disc) -> QPointF {
  const qreal   radius = disc.width() * 0.5;
  const QPointF center = disc.center();
  const QPointF p      = color_wheel::ClampDiscPoint(normalized);
  return QPointF(center.x() + p.x() * radius, center.y() - p.y() * radius);
}

auto CdlTrackballToNormalizedPoint(const QPointF& widget_point, const QRectF& disc) -> QPointF {
  const qreal radius = disc.width() * 0.5;
  if (radius <= 0.0) {
    return QPointF(0.0, 0.0);
  }
  const QPointF center = disc.center();
  const qreal   x      = (widget_point.x() - center.x()) / radius;
  const qreal   y      = (center.y() - widget_point.y()) / radius;
  return color_wheel::ClampDiscPoint(QPointF(x, y));
}

struct EditorCdlTrackballItem::DiscRootNode : public QSGNode {
  QSGSimpleTextureNode* texture       = nullptr;
  QSGGeometryNode*      rim           = nullptr;
  QSGGeometryNode*      cross         = nullptr;
  QSGGeometryNode*      handle        = nullptr;
  QSGGeometryNode*      outline       = nullptr;
  QSGTexture*           owned_texture = nullptr;

  ~DiscRootNode() override {
    delete owned_texture;
    owned_texture = nullptr;
  }
};

EditorCdlTrackballItem::EditorCdlTrackballItem(QQuickItem* parent) : QQuickItem(parent) {
  setFlag(ItemHasContents, true);
  setAcceptedMouseButtons(Qt::LeftButton);
  setAcceptHoverEvents(true);
  setCursor(Qt::CrossCursor);
  setAntialiasing(true);
  setSmooth(true);
  setImplicitWidth(140);
  setImplicitHeight(140);
}

void EditorCdlTrackballItem::setModel(EditorCdlTrackballModel* model) {
  if (model_ == model) {
    return;
  }
  bindModel(model);
  emit modelChanged();
  rebuildGeometry();
}

void EditorCdlTrackballItem::setWheelRole(const QString& role) {
  if (wheel_role_ == role) {
    return;
  }
  wheel_role_ = role;
  emit wheelRoleChanged();
  rebuildGeometry();
}

void EditorCdlTrackballItem::setBackgroundColor(const QColor& c) {
  if (background_color_ == c) {
    return;
  }
  background_color_ = c;
  emit colorsChanged();
  markGeometryDirty();
}

void EditorCdlTrackballItem::setRimColor(const QColor& c) {
  if (rim_color_ == c) {
    return;
  }
  rim_color_ = c;
  emit colorsChanged();
  markGeometryDirty();
}

void EditorCdlTrackballItem::setCrosshairColor(const QColor& c) {
  if (crosshair_color_ == c) {
    return;
  }
  crosshair_color_ = c;
  emit colorsChanged();
  markGeometryDirty();
}

void EditorCdlTrackballItem::setHandleColor(const QColor& c) {
  if (handle_color_ == c) {
    return;
  }
  handle_color_ = c;
  emit colorsChanged();
  markGeometryDirty();
}

void EditorCdlTrackballItem::setHandleOutlineColor(const QColor& c) {
  if (handle_outline_color_ == c) {
    return;
  }
  handle_outline_color_ = c;
  emit colorsChanged();
  markGeometryDirty();
}

void EditorCdlTrackballItem::refreshGeometry() { rebuildGeometry(); }

void EditorCdlTrackballItem::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
  QQuickItem::geometryChange(newGeometry, oldGeometry);
  if (newGeometry.size() != oldGeometry.size()) {
    wheel_cache_ = QImage();
    rebuildGeometry();
  }
}

void EditorCdlTrackballItem::bindModel(EditorCdlTrackballModel* model) {
  if (model_) {
    disconnect(model_, nullptr, this, nullptr);
  }
  model_ = model;
  if (!model_) {
    return;
  }
  connect(model_, &EditorCdlTrackballModel::liftChanged, this,
          &EditorCdlTrackballItem::onModelWheelChanged);
  connect(model_, &EditorCdlTrackballModel::gammaChanged, this,
          &EditorCdlTrackballItem::onModelWheelChanged);
  connect(model_, &EditorCdlTrackballModel::gainChanged, this,
          &EditorCdlTrackballItem::onModelWheelChanged);
  connect(model_, &EditorCdlTrackballModel::enabledChanged, this,
          [this]() { setEnabled(model_ ? model_->enabled() : true); });
  setEnabled(model_->enabled());
}

void EditorCdlTrackballItem::onModelWheelChanged() { rebuildGeometry(); }

auto EditorCdlTrackballItem::currentDiscPosition() const -> QPointF {
  if (!model_) {
    return QPointF(0.0, 0.0);
  }
  if (wheel_role_.compare(QStringLiteral("gamma"), Qt::CaseInsensitive) == 0) {
    return QPointF(model_->gammaX(), model_->gammaY());
  }
  if (wheel_role_.compare(QStringLiteral("gain"), Qt::CaseInsensitive) == 0) {
    return QPointF(model_->gainX(), model_->gainY());
  }
  return QPointF(model_->liftX(), model_->liftY());
}

void EditorCdlTrackballItem::rebuildGeometry() {
  last_disc_rect_    = CdlTrackballDiscRect(width(), height());
  last_handle_point_ = CdlTrackballToWidgetPoint(currentDiscPosition(), last_disc_rect_);
  // Supersample at least 2× (or the window DPR) so the disc rim and hue field
  // stay anti-aliased when downscaled into the item rect.
  const qreal dpr =
      (window() != nullptr) ? std::max<qreal>(2.0, window()->effectiveDevicePixelRatio()) : 2.0;
  const int size = std::max(1, static_cast<int>(std::lround(last_disc_rect_.width() * dpr)));
  if (wheel_cache_.isNull() || wheel_cache_.width() != size) {
    wheel_cache_ = BuildWheelImage(size);
  }
  ++geometry_revision_;
  geometry_dirty_ = true;
  emit geometryRevisionChanged();
  update();
}

void EditorCdlTrackballItem::markGeometryDirty() {
  geometry_dirty_ = true;
  update();
}

auto EditorCdlTrackballItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData*) -> QSGNode* {
  auto* root = static_cast<DiscRootNode*>(old_node);
  if (!root) {
    root = new DiscRootNode;
  }
  if (!geometry_dirty_ && old_node) {
    return root;
  }
  geometry_dirty_ = false;

  if (!root->texture) {
    root->texture = new QSGSimpleTextureNode;
    root->appendChildNode(root->texture);
  }
  if (window() && !wheel_cache_.isNull()) {
    delete root->owned_texture;
    root->owned_texture =
        window()->createTextureFromImage(wheel_cache_, QQuickWindow::TextureHasAlphaChannel);
    root->texture->setTexture(root->owned_texture);
    root->texture->setRect(last_disc_rect_);
    root->texture->setFiltering(QSGTexture::Linear);
  }

  // Rim is soft-edged inside the supersampled texture; keep a light stroke for
  // contrast on dark panels without introducing hard 1-px jaggies.
  std::vector<QPointF> rim_segs;
  AppendRing(rim_segs, last_disc_rect_, 96);
  UpsertSolidNode(root, root->rim, rim_segs, rim_color_, QSGGeometry::DrawLines, 1.0);

  std::vector<QPointF> cross;
  const QPointF        c = last_disc_rect_.center();
  cross.emplace_back(c.x() - 6.0, c.y());
  cross.emplace_back(c.x() + 6.0, c.y());
  cross.emplace_back(c.x(), c.y() - 6.0);
  cross.emplace_back(c.x(), c.y() + 6.0);
  UpsertSolidNode(root, root->cross, cross, crosshair_color_, QSGGeometry::DrawLines, 1.0);

  std::vector<QPointF> handle_tris;
  AppendDisc(handle_tris, last_handle_point_, 6.5);
  UpsertSolidNode(root, root->handle, handle_tris, handle_color_, QSGGeometry::DrawTriangles);

  std::vector<QPointF> outline_segs;
  AppendRing(outline_segs,
             QRectF(last_handle_point_.x() - 9.5, last_handle_point_.y() - 9.5, 19.0, 19.0), 48);
  UpsertSolidNode(root, root->outline, outline_segs, handle_outline_color_, QSGGeometry::DrawLines,
                  1.0);

  return root;
}

void EditorCdlTrackballItem::mousePressEvent(QMouseEvent* event) {
  if (!model_ || !model_->enabled() || event->button() != Qt::LeftButton) {
    QQuickItem::mousePressEvent(event);
    return;
  }
  dragging_  = true;
  discMoved_ = false;
  // Click-to-position: sample on press. updateDiscDrag only marks the gesture
  // moved (and submits) when the disc value actually changes — so a double-click
  // on an unchanged point does not settle before resetWheel.
  model_->beginDiscDrag(wheel_role_);
  const QPointF n = CdlTrackballToNormalizedPoint(event->position(), last_disc_rect_);
  model_->updateDiscDrag(wheel_role_, n.x(), n.y());
  event->accept();
}

void EditorCdlTrackballItem::mouseMoveEvent(QMouseEvent* event) {
  if (!dragging_ || !model_) {
    QQuickItem::mouseMoveEvent(event);
    return;
  }
  const QPointF n = CdlTrackballToNormalizedPoint(event->position(), last_disc_rect_);
  discMoved_      = true;
  model_->updateDiscDrag(wheel_role_, n.x(), n.y());
  event->accept();
}

void EditorCdlTrackballItem::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton || !dragging_) {
    QQuickItem::mouseReleaseEvent(event);
    return;
  }
  dragging_ = false;
  if (model_) {
    if (discMoved_) {
      const QPointF n = CdlTrackballToNormalizedPoint(event->position(), last_disc_rect_);
      model_->updateDiscDrag(wheel_role_, n.x(), n.y());
    }
    model_->finishDiscDrag();
  }
  discMoved_ = false;
  event->accept();
}

void EditorCdlTrackballItem::mouseDoubleClickEvent(QMouseEvent* event) {
  if (!model_ || !model_->enabled() || event->button() != Qt::LeftButton) {
    QQuickItem::mouseDoubleClickEvent(event);
    return;
  }
  // Abort any open press/drag from the double-click sequence. resetWheel()
  // clears model drag state and commits the identity wheel once — do not call
  // finishDiscDrag here (that would settle the pre-reset disc position first).
  dragging_  = false;
  discMoved_ = false;
  model_->resetWheel(wheel_role_);
  event->accept();
}

}  // namespace alcedo::ui
