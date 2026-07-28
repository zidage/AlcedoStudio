//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_scope_item.hpp"

#include <qqml.h>

#include <QSGFlatColorMaterial>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGNode>
#include <QSGVertexColorMaterial>
#include <algorithm>
#include <array>
#include <cmath>

#include "ui/alcedo_main/album_backend/editor_scope_controller.hpp"

namespace alcedo::ui {
namespace {

auto PlotRect(qreal width, qreal height) -> QRectF {
  constexpr qreal kInset = 1.0;
  return QRectF(kInset, kInset, std::max<qreal>(1.0, width - 2.0 * kInset),
                std::max<qreal>(1.0, height - 2.0 * kInset));
}

void AppendRectangle(std::vector<QPointF>& points, const QRectF& rect) {
  points.push_back(rect.topLeft());
  points.push_back(rect.topRight());
  points.push_back(rect.bottomRight());
  points.push_back(rect.topLeft());
  points.push_back(rect.bottomRight());
  points.push_back(rect.bottomLeft());
}

void FillSolidGeometry(QSGGeometry* geometry, const std::vector<QPointF>& points,
                       QSGGeometry::DrawingMode mode) {
  geometry->allocate(static_cast<int>(points.size()));
  geometry->setDrawingMode(mode);
  auto* vertices = geometry->vertexDataAsPoint2D();
  for (size_t i = 0; i < points.size(); ++i) {
    vertices[i].set(static_cast<float>(points[i].x()), static_cast<float>(points[i].y()));
  }
  geometry->markVertexDataDirty();
}

void UpsertSolidNode(QSGNode* root, QSGGeometryNode*& slot, const std::vector<QPointF>& points,
                     const QColor& color, QSGGeometry::DrawingMode mode) {
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
  FillSolidGeometry(slot->geometry(), points, mode);
  static_cast<QSGFlatColorMaterial*>(slot->material())->setColor(color);
  slot->markDirty(QSGNode::DirtyGeometry | QSGNode::DirtyMaterial);
}

void UpsertWaveformNode(QSGNode* root, QSGGeometryNode*& slot,
                        const alcedo::ScopeWaveformRenderData& waveform, const QRectF& plot,
                        const QColor& red, const QColor& green, const QColor& blue) {
  std::vector<QPointF> points;
  std::vector<QColor>  colors;
  if (waveform.valid && waveform.width > 0 && waveform.height > 0) {
    const size_t expected =
        static_cast<size_t>(waveform.width) * static_cast<size_t>(waveform.height) * 4U;
    if (waveform.rgba.size() >= expected) {
      points.reserve(static_cast<size_t>(waveform.width) * static_cast<size_t>(waveform.height));
      colors.reserve(points.capacity());
      for (int y = 0; y < waveform.height; ++y) {
        for (int x = 0; x < waveform.width; ++x) {
          const size_t index = (static_cast<size_t>(y) * static_cast<size_t>(waveform.width) +
                                static_cast<size_t>(x)) *
                               4U;
          const float r      = std::clamp(waveform.rgba[index], 0.0f, 1.0f);
          const float g      = std::clamp(waveform.rgba[index + 1U], 0.0f, 1.0f);
          const float b      = std::clamp(waveform.rgba[index + 2U], 0.0f, 1.0f);
          const float weight = std::max({r, g, b});
          if (weight <= 0.01f) {
            continue;
          }
          const qreal px = plot.left() +
                           (static_cast<qreal>(x) / std::max(1, waveform.width - 1)) * plot.width();
          const qreal py = plot.top() + (static_cast<qreal>(y) / std::max(1, waveform.height - 1)) *
                                            plot.height();
          points.emplace_back(px, py);
          const auto channel = [weight](const QColor& tint, float value) {
            return std::clamp(static_cast<int>(std::lround(tint.redF() * value / weight * 255.0f)),
                              0, 255);
          };
          const int out_r =
              std::clamp(channel(red, r) + channel(green, g) + channel(blue, b), 0, 255);
          const int out_g =
              std::clamp(static_cast<int>(std::lround(red.greenF() * r / weight * 255.0f)) +
                             static_cast<int>(std::lround(green.greenF() * g / weight * 255.0f)) +
                             static_cast<int>(std::lround(blue.greenF() * b / weight * 255.0f)),
                         0, 255);
          const int out_b =
              std::clamp(static_cast<int>(std::lround(red.blueF() * r / weight * 255.0f)) +
                             static_cast<int>(std::lround(green.blueF() * g / weight * 255.0f)) +
                             static_cast<int>(std::lround(blue.blueF() * b / weight * 255.0f)),
                         0, 255);
          colors.emplace_back(out_r, out_g, out_b,
                              std::clamp(static_cast<int>(std::lround(weight * 220.0f)), 24, 220));
        }
      }
    }
  }

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
    auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(),
                                     static_cast<int>(points.size()));
    slot->setGeometry(geometry);
    slot->setFlag(QSGNode::OwnsGeometry);
    auto* material = new QSGVertexColorMaterial;
    slot->setMaterial(material);
    slot->setFlag(QSGNode::OwnsMaterial);
    root->appendChildNode(slot);
  }

  auto* geometry = slot->geometry();
  geometry->allocate(static_cast<int>(points.size()));
  geometry->setDrawingMode(QSGGeometry::DrawPoints);
  auto* vertices = geometry->vertexDataAsColoredPoint2D();
  for (size_t i = 0; i < points.size(); ++i) {
    const auto& color = colors[i];
    vertices[i].set(static_cast<float>(points[i].x()), static_cast<float>(points[i].y()),
                    static_cast<uchar>(color.red()), static_cast<uchar>(color.green()),
                    static_cast<uchar>(color.blue()), static_cast<uchar>(color.alpha()));
  }
  geometry->markVertexDataDirty();
  slot->markDirty(QSGNode::DirtyGeometry | QSGNode::DirtyMaterial);
}

}  // namespace

struct EditorScopeItem::ScopeRootNode : public QSGNode {
  QSGGeometryNode* background   = nullptr;
  QSGGeometryNode* grid         = nullptr;
  QSGGeometryNode* border       = nullptr;
  QSGGeometryNode* histogram[3] = {nullptr, nullptr, nullptr};
  QSGGeometryNode* waveform     = nullptr;
};

EditorScopeItem::EditorScopeItem(QQuickItem* parent) : QQuickItem(parent) {
  setFlag(ItemHasContents, true);
  setClip(true);
  setImplicitWidth(240);
  setImplicitHeight(120);
}

void EditorScopeItem::set_controller(QObject* controller) {
  if (controller_ == controller) {
    return;
  }
  auto* typed_controller = qobject_cast<EditorScopeController*>(controller);
  bind_controller(typed_controller);
  controller_ = typed_controller;
  emit ControllerChanged();
  sync_snapshot();
}

void EditorScopeItem::set_background_color(const QColor& color) {
  if (background_color_ == color) {
    return;
  }
  background_color_ = color;
  emit ColorsChanged();
  mark_scene_dirty();
}

void EditorScopeItem::set_grid_color(const QColor& color) {
  if (grid_color_ == color) {
    return;
  }
  grid_color_ = color;
  emit ColorsChanged();
  mark_scene_dirty();
}

void EditorScopeItem::set_border_color(const QColor& color) {
  if (border_color_ == color) {
    return;
  }
  border_color_ = color;
  emit ColorsChanged();
  mark_scene_dirty();
}

void EditorScopeItem::set_histogram_red_color(const QColor& color) {
  if (histogram_red_color_ == color) {
    return;
  }
  histogram_red_color_ = color;
  emit ColorsChanged();
  mark_scene_dirty();
}

void EditorScopeItem::set_histogram_green_color(const QColor& color) {
  if (histogram_green_color_ == color) {
    return;
  }
  histogram_green_color_ = color;
  emit ColorsChanged();
  mark_scene_dirty();
}

void EditorScopeItem::set_histogram_blue_color(const QColor& color) {
  if (histogram_blue_color_ == color) {
    return;
  }
  histogram_blue_color_ = color;
  emit ColorsChanged();
  mark_scene_dirty();
}

void EditorScopeItem::RegisterQmlType() {
  qmlRegisterType<EditorScopeItem>("Alcedo.Main", 1, 0, "EditorScopeItem");
}

void EditorScopeItem::bind_controller(EditorScopeController* controller) {
  if (snapshot_connection_) {
    QObject::disconnect(snapshot_connection_);
    snapshot_connection_ = {};
  }
  if (active_view_connection_) {
    QObject::disconnect(active_view_connection_);
    active_view_connection_ = {};
  }
  if (controller_destroyed_connection_) {
    QObject::disconnect(controller_destroyed_connection_);
    controller_destroyed_connection_ = {};
  }
  scope_controller_ = controller;
  if (!scope_controller_) {
    return;
  }
  snapshot_connection_ = connect(scope_controller_, &EditorScopeController::SnapshotChanged, this,
                                 &EditorScopeItem::sync_snapshot);
  active_view_connection_ = connect(scope_controller_, &EditorScopeController::ActiveViewChanged,
                                    this, &EditorScopeItem::mark_scene_dirty);
  controller_destroyed_connection_ = connect(scope_controller_, &QObject::destroyed, this, [this] {
    controller_                      = nullptr;
    scope_controller_                = nullptr;
    snapshot_connection_             = {};
    active_view_connection_          = {};
    snapshot_                        = {};
    controller_destroyed_connection_ = {};
    mark_scene_dirty();
    emit ControllerChanged();
  });
}

void EditorScopeItem::sync_snapshot() {
  snapshot_ = scope_controller_ ? scope_controller_->snapshot() : alcedo::ScopeRenderSnapshot{};
  mark_scene_dirty();
}

void EditorScopeItem::mark_scene_dirty() {
  scene_dirty_ = true;
  update();
}

void EditorScopeItem::geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) {
  QQuickItem::geometryChange(new_geometry, old_geometry);
  if (new_geometry.size() != old_geometry.size()) {
    mark_scene_dirty();
  }
}

auto EditorScopeItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData*) -> QSGNode* {
  auto* root = static_cast<ScopeRootNode*>(old_node);
  if (!root) {
    root = new ScopeRootNode;
  }
  if (!scene_dirty_ && old_node) {
    return root;
  }
  scene_dirty_              = false;

  const QRectF         plot = PlotRect(width(), height());
  std::vector<QPointF> background;
  AppendRectangle(background, QRectF(QPointF(0.0, 0.0), QSizeF(width(), height())));
  UpsertSolidNode(root, root->background, background, background_color_,
                  QSGGeometry::DrawTriangles);

  std::vector<QPointF> grid;
  for (int i = 1; i < 4; ++i) {
    const qreal t = static_cast<qreal>(i) / 4.0;
    const qreal x = plot.left() + t * plot.width();
    const qreal y = plot.top() + t * plot.height();
    grid.emplace_back(x, plot.top());
    grid.emplace_back(x, plot.bottom());
    grid.emplace_back(plot.left(), y);
    grid.emplace_back(plot.right(), y);
  }
  UpsertSolidNode(root, root->grid, grid, grid_color_, QSGGeometry::DrawLines);

  const std::vector<QPointF> border = {plot.topLeft(), plot.topRight(), plot.bottomLeft(),
                                       plot.bottomRight()};
  UpsertSolidNode(root, root->border, border, border_color_, QSGGeometry::DrawLines);

  const int active_view = scope_controller_ ? scope_controller_->active_view() : 0;
  if (active_view == 1) {
    UpsertSolidNode(root, root->histogram[0], {}, histogram_red_color_, QSGGeometry::DrawLineStrip);
    UpsertSolidNode(root, root->histogram[1], {}, histogram_green_color_,
                    QSGGeometry::DrawLineStrip);
    UpsertSolidNode(root, root->histogram[2], {}, histogram_blue_color_,
                    QSGGeometry::DrawLineStrip);
    UpsertWaveformNode(root, root->waveform, snapshot_.waveform, plot, histogram_red_color_,
                       histogram_green_color_, histogram_blue_color_);
  } else {
    UpsertWaveformNode(root, root->waveform, alcedo::ScopeWaveformRenderData{}, plot,
                       histogram_red_color_, histogram_green_color_, histogram_blue_color_);
    std::array<std::vector<QPointF>, 3> lines;
    if (snapshot_.histogram.valid && snapshot_.histogram.bins > 1 &&
        snapshot_.histogram.rgb.size() >= static_cast<size_t>(snapshot_.histogram.bins) * 3U) {
      for (int channel = 0; channel < 3; ++channel) {
        auto& line = lines[static_cast<size_t>(channel)];
        line.reserve(static_cast<size_t>(snapshot_.histogram.bins));
        for (int bin = 0; bin < snapshot_.histogram.bins; ++bin) {
          const float value =
              std::clamp(snapshot_.histogram
                             .rgb[static_cast<size_t>(channel * snapshot_.histogram.bins + bin)],
                         0.0f, 1.0f);
          const qreal x = plot.left() + (static_cast<qreal>(bin) /
                                         static_cast<qreal>(snapshot_.histogram.bins - 1)) *
                                            plot.width();
          const qreal y = plot.bottom() - static_cast<qreal>(value) * plot.height();
          line.emplace_back(x, y);
        }
      }
    }
    UpsertSolidNode(root, root->histogram[0], lines[0], histogram_red_color_,
                    QSGGeometry::DrawLineStrip);
    UpsertSolidNode(root, root->histogram[1], lines[1], histogram_green_color_,
                    QSGGeometry::DrawLineStrip);
    UpsertSolidNode(root, root->histogram[2], lines[2], histogram_blue_color_,
                    QSGGeometry::DrawLineStrip);
  }
  return root;
}

}  // namespace alcedo::ui
