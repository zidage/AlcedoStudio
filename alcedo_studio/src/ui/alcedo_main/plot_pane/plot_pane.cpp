//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/plot_pane/plot_pane.hpp"

#include <qqml.h>

#include <QCursor>
#include <QHoverEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQmlInfo>
#include <QQuickWindow>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGNode>
#include <QSGTextNode>
#include <QSGVertexColorMaterial>
#include <QTextLine>
#include <QTextOption>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "ui/alcedo_main/plot_pane/plot_grid_layer.hpp"
#include "ui/alcedo_main/plot_pane/plot_handle_layer.hpp"
#include "ui/alcedo_main/plot_pane/plot_histogram_layer.hpp"
#include "ui/alcedo_main/plot_pane/plot_polyline_layer.hpp"
#include "ui/alcedo_main/plot_pane/plot_primitives.hpp"

namespace alcedo::ui {
namespace {

// Angle delta of one wheel notch.
constexpr double kWheelNotch     = 120.0;
// Width given to a label line; labels do not wrap.
constexpr qreal  kLabelLineWidth = 100000.0;

auto             ToDataRect(const QRectF& rect) -> PlotDataRect {
  return {{rect.x(), rect.x() + rect.width()}, {rect.y(), rect.y() + rect.height()}};
}

auto ToQRect(const PlotDataRect& rect) -> QRectF {
  return {rect.x.minimum, rect.y.minimum, rect.x.span(), rect.y.span()};
}

// Puts @p triangles into the geometry node under @p container. The node exists only while there
// are triangles: the batch renderer does not draw a geometry node that it first saw without
// vertices, even after the vertices arrive.
void UploadTriangles(QSGNode* container, const std::vector<MaskOverlayVertex>& triangles) {
  auto* node = static_cast<QSGGeometryNode*>(container->firstChild());
  if (triangles.empty()) {
    if (node) {
      container->removeChildNode(node);
      delete node;
    }
    return;
  }
  if (!node) {
    node           = new QSGGeometryNode;
    auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(),
                                     static_cast<int>(triangles.size()));
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    node->setGeometry(geometry);
    node->setFlag(QSGNode::OwnsGeometry);
    node->setMaterial(new QSGVertexColorMaterial);
    node->setFlag(QSGNode::OwnsMaterial);
    container->appendChildNode(node);
  }
  QSGGeometry* geometry = node->geometry();
  geometry->allocate(static_cast<int>(triangles.size()));
  auto* vertices = geometry->vertexDataAsColoredPoint2D();
  for (size_t i = 0; i < triangles.size(); ++i) {
    const auto& v = triangles[i];
    vertices[i].set(v.x, v.y, v.r, v.g, v.b, v.a);
  }
  geometry->markVertexDataDirty();
  node->markDirty(QSGNode::DirtyGeometry);
}

}  // namespace

struct PlotPane::PaneRootNode : public QSGNode {
  // A container holds the geometry node of its triangles while there are triangles, so the
  // drawing order of the layers stays fixed.
  struct LayerNodes {
    QSGNode*     triangles = nullptr;
    QSGTextNode* text      = nullptr;
  };
  QSGNode*                base = nullptr;
  std::vector<LayerNodes> layers{};
};

PlotPane::PlotPane(QQuickItem* parent) : QQuickItem(parent) {
  setFlag(ItemHasContents, true);
  setAcceptedMouseButtons(Qt::LeftButton);
  setAcceptHoverEvents(true);
  setClip(true);
  setImplicitWidth(240);
  setImplicitHeight(240);
  view_ = PlotViewTransform(PlotDataRect{}, PlotViewTransform::kDefaultMaximumZoom);
}

PlotPane::~PlotPane() {
  for (auto& slot : slots_) {
    QObject::disconnect(slot->content_connection);
    QObject::disconnect(slot->destroyed_connection);
  }
}

// ---- Layers ----------------------------------------------------------------------------------

void PlotPane::AddLayer(PlotLayer* layer) {
  if (!layer || FindSlot(layer)) {
    return;
  }
  auto  slot = std::make_unique<LayerSlot>();
  auto* raw  = slot.get();
  raw->layer = layer;
  raw->content_connection =
      connect(layer, &PlotLayer::contentChanged, this, [this, raw] { MarkLayerDirty(*raw); });
  raw->destroyed_connection =
      connect(layer, &QObject::destroyed, this, [this, layer] { RemoveSlot(layer); });
  slots_.push_back(std::move(slot));
  layer_list_changed_ = true;
  MarkLayerDirty(*raw);
}

void PlotPane::ClearLayers() {
  for (auto& slot : slots_) {
    QObject::disconnect(slot->content_connection);
    QObject::disconnect(slot->destroyed_connection);
  }
  slots_.clear();
  grab_layer_         = nullptr;
  hover_layer_        = nullptr;
  layer_list_changed_ = true;
  update();
}

auto PlotPane::layer_at(int index) const -> PlotLayer* {
  if (index < 0 || index >= layer_count()) {
    return nullptr;
  }
  return slots_[static_cast<size_t>(index)]->layer;
}

auto PlotPane::layers_property() -> QQmlListProperty<PlotLayer> {
  return {this,
          nullptr,
          &PlotPane::AppendLayer,
          &PlotPane::LayerCount,
          &PlotPane::LayerAt,
          &PlotPane::ClearLayerList};
}

void PlotPane::AppendLayer(QQmlListProperty<PlotLayer>* list, PlotLayer* layer) {
  static_cast<PlotPane*>(list->object)->AddLayer(layer);
}

auto PlotPane::LayerCount(QQmlListProperty<PlotLayer>* list) -> qsizetype {
  return static_cast<PlotPane*>(list->object)->layer_count();
}

auto PlotPane::LayerAt(QQmlListProperty<PlotLayer>* list, qsizetype index) -> PlotLayer* {
  return static_cast<PlotPane*>(list->object)->layer_at(static_cast<int>(index));
}

void PlotPane::ClearLayerList(QQmlListProperty<PlotLayer>* list) {
  static_cast<PlotPane*>(list->object)->ClearLayers();
}

auto PlotPane::FindSlot(const PlotLayer* layer) const -> LayerSlot* {
  for (const auto& slot : slots_) {
    if (slot->layer == layer) {
      return slot.get();
    }
  }
  return nullptr;
}

void PlotPane::RemoveSlot(const PlotLayer* layer) {
  const auto it = std::find_if(slots_.begin(), slots_.end(),
                               [layer](const auto& slot) { return slot->layer == layer; });
  if (it == slots_.end()) {
    return;
  }
  QObject::disconnect((*it)->content_connection);
  QObject::disconnect((*it)->destroyed_connection);
  slots_.erase(it);
  if (grab_layer_ == layer) {
    grab_layer_ = nullptr;
    setKeepMouseGrab(false);
  }
  if (hover_layer_ == layer) {
    hover_layer_ = nullptr;
  }
  layer_list_changed_ = true;
  UpdateCursor();
  update();
}

void PlotPane::MarkLayerDirty(LayerSlot& slot) {
  slot.dirty = true;
  polish();
  update();
}

void PlotPane::MarkAllLayersDirty() {
  for (auto& slot : slots_) {
    slot->dirty = true;
  }
  polish();
  update();
}

// ---- View ------------------------------------------------------------------------------------

auto PlotPane::limits() const -> QRectF { return ToQRect(view_.limits()); }

void PlotPane::set_limits(const QRectF& limits) {
  const PlotDataRect data = ToDataRect(limits);
  if (data == view_.limits()) {
    return;
  }
  try {
    view_.SetLimits(data);
  } catch (const std::invalid_argument& error) {
    qmlWarning(this) << "PlotPane.limits: " << error.what();
    return;
  }
  emit limitsChanged();
  OnViewChanged();
}

void PlotPane::set_maximum_zoom(qreal maximum_zoom) {
  if (maximum_zoom == view_.maximum_zoom()) {
    return;
  }
  const PlotDataRect before = view_.view();
  try {
    view_.SetMaximumZoom(maximum_zoom);
  } catch (const std::invalid_argument& error) {
    qmlWarning(this) << "PlotPane.maximumZoom: " << error.what();
    return;
  }
  emit limitsChanged();
  if (view_.view() != before) {
    OnViewChanged();
  }
}

auto PlotPane::view() const -> QRectF { return ToQRect(view_.view()); }

void PlotPane::setView(const QRectF& view) {
  if (view_.SetView(ToDataRect(view))) {
    OnViewChanged();
  }
}

void PlotPane::resetView() {
  if (view_.ResetView()) {
    OnViewChanged();
  }
}

void PlotPane::zoomAt(const QPointF& data_anchor, qreal factor) {
  if (view_.ZoomAt(data_anchor, factor)) {
    OnViewChanged();
  }
}

auto PlotPane::mapToData(const QPointF& item) const -> QPointF { return view_.ToData(item); }

auto PlotPane::mapFromData(const QPointF& data) const -> QPointF { return view_.ToItem(data); }

void PlotPane::OnViewChanged() {
  MarkAllLayersDirty();
  UpdateCursor();
  emit viewChanged();
  if (pointer_inside_) {
    // The same item position shows another data position.
    emit pointerChanged();
  }
}

auto PlotPane::pointer_position() const -> QPointF { return view_.ToData(pointer_item_); }

void PlotPane::SetPointer(const QPointF& item, bool inside) {
  inside = inside && view_.item_rect().contains(item);
  if (inside == pointer_inside_ && (!inside || item == pointer_item_)) {
    return;
  }
  pointer_inside_ = inside;
  if (inside) {
    pointer_item_ = item;
  }
  emit pointerChanged();
}

void PlotPane::UpdatePlotRect() {
  const qreal  w = std::max<qreal>(0.0, width() - inset_left_ - inset_right_);
  const qreal  h = std::max<qreal>(0.0, height() - inset_top_ - inset_bottom_);
  const QRectF rect(inset_left_, inset_top_, w, h);
  if (rect == view_.item_rect()) {
    return;
  }
  view_.SetItemRect(rect);
  base_changed_ = true;
  MarkAllLayersDirty();
  emit plotRectChanged();
}

void PlotPane::geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) {
  QQuickItem::geometryChange(new_geometry, old_geometry);
  if (new_geometry.size() != old_geometry.size()) {
    base_changed_ = true;
    UpdatePlotRect();
    update();
  }
}

// ---- Style -----------------------------------------------------------------------------------

void PlotPane::set_inset_left(qreal inset) {
  if (inset_left_ == inset) {
    return;
  }
  inset_left_ = inset;
  emit insetsChanged();
  UpdatePlotRect();
}

void PlotPane::set_inset_top(qreal inset) {
  if (inset_top_ == inset) {
    return;
  }
  inset_top_ = inset;
  emit insetsChanged();
  UpdatePlotRect();
}

void PlotPane::set_inset_right(qreal inset) {
  if (inset_right_ == inset) {
    return;
  }
  inset_right_ = inset;
  emit insetsChanged();
  UpdatePlotRect();
}

void PlotPane::set_inset_bottom(qreal inset) {
  if (inset_bottom_ == inset) {
    return;
  }
  inset_bottom_ = inset;
  emit insetsChanged();
  UpdatePlotRect();
}

void PlotPane::set_background_color(const QColor& color) {
  if (background_color_ == color) {
    return;
  }
  background_color_ = color;
  base_changed_     = true;
  emit colorsChanged();
  update();
}

void PlotPane::set_surface_color(const QColor& color) {
  if (surface_color_ == color) {
    return;
  }
  surface_color_ = color;
  base_changed_  = true;
  emit colorsChanged();
  update();
}

void PlotPane::set_wheel_zoom_step(qreal step) {
  if (wheel_zoom_step_ == step || !(step > 1.0) || !std::isfinite(step)) {
    return;
  }
  wheel_zoom_step_ = step;
  emit wheelZoomStepChanged();
}

// ---- Build and upload ------------------------------------------------------------------------

void PlotPane::BuildDirtyLayers() {
  bool built = false;
  for (auto& slot : slots_) {
    if (!slot->dirty) {
      continue;
    }
    slot->layer->Build(view_, slot->geometry);
    BuildLabels(*slot);
    slot->dirty        = false;
    slot->needs_upload = true;
    ++slot->build_count;
    built = true;
  }
  if (built) {
    update();
  }
}

void PlotPane::BuildLabels(LayerSlot& slot) const {
  slot.text_layouts.clear();
  slot.text_positions.clear();
  QTextOption option;
  option.setWrapMode(QTextOption::NoWrap);
  for (const auto& label : slot.geometry.labels) {
    auto layout = std::make_unique<QTextLayout>(label.text, slot.geometry.label_font);
    layout->setTextOption(option);
    layout->setCacheEnabled(true);
    layout->beginLayout();
    QTextLine line = layout->createLine();
    if (!line.isValid()) {
      layout->endLayout();
      continue;
    }
    line.setLineWidth(kLabelLineWidth);
    line.setPosition({0.0, 0.0});
    layout->endLayout();
    const qreal w = line.naturalTextWidth();
    const qreal h = line.height();
    qreal       x = label.anchor.x();
    qreal       y = label.anchor.y();
    if (label.alignment & Qt::AlignHCenter) {
      x -= 0.5 * w;
    } else if (label.alignment & Qt::AlignRight) {
      x -= w;
    }
    if (label.alignment & Qt::AlignVCenter) {
      y -= 0.5 * h;
    } else if (label.alignment & Qt::AlignBottom) {
      y -= h;
    }
    slot.text_positions.emplace_back(std::round(x), std::round(y));
    slot.text_layouts.push_back(std::move(layout));
  }
}

auto PlotPane::LayerBuildCount(const PlotLayer* layer) const -> int {
  const LayerSlot* slot = FindSlot(layer);
  return slot ? slot->build_count : -1;
}

auto PlotPane::LayerGeometry(const PlotLayer* layer) const -> const PlotLayerGeometry* {
  const LayerSlot* slot = FindSlot(layer);
  return slot ? &slot->geometry : nullptr;
}

void PlotPane::updatePolish() { BuildDirtyLayers(); }

auto PlotPane::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData*) -> QSGNode* {
  auto* root = static_cast<PaneRootNode*>(old_node);
  if (!root) {
    root       = new PaneRootNode;
    root->base = new QSGNode;
    root->appendChildNode(root->base);
    base_changed_       = true;
    layer_list_changed_ = true;
  }
  // The layers were built in updatePolish on the GUI thread; this sync only uploads them.
  if (base_changed_) {
    std::vector<MaskOverlayVertex> base;
    if (background_color_.alpha() > 0) {
      plot_primitives::AppendRect(base, boundingRect(), background_color_);
    }
    if (surface_color_.alpha() > 0) {
      plot_primitives::AppendRect(base, view_.item_rect(), surface_color_);
    }
    UploadTriangles(root->base, base);
    base_changed_ = false;
  }

  if (layer_list_changed_) {
    for (auto& nodes : root->layers) {
      root->removeChildNode(nodes.triangles);
      delete nodes.triangles;
      if (nodes.text) {
        root->removeChildNode(nodes.text);
        delete nodes.text;
      }
    }
    root->layers.clear();
    for (auto& slot : slots_) {
      PaneRootNode::LayerNodes nodes;
      nodes.triangles = new QSGNode;
      root->appendChildNode(nodes.triangles);
      if (window()) {
        nodes.text = window()->createTextNode();
        root->appendChildNode(nodes.text);
      }
      root->layers.push_back(nodes);
      slot->needs_upload = true;
    }
    layer_list_changed_ = false;
  }

  for (size_t i = 0; i < slots_.size() && i < root->layers.size(); ++i) {
    LayerSlot& slot = *slots_[i];
    if (!slot.needs_upload) {
      continue;
    }
    const auto& nodes = root->layers[i];
    UploadTriangles(nodes.triangles, slot.geometry.triangles);
    if (nodes.text) {
      nodes.text->clear();
      nodes.text->setColor(slot.geometry.label_color);
      for (size_t t = 0; t < slot.text_layouts.size(); ++t) {
        nodes.text->addTextLayout(slot.text_positions[t], slot.text_layouts[t].get());
      }
    }
    slot.needs_upload = false;
  }
  return root;
}

// ---- Input -----------------------------------------------------------------------------------

auto PlotPane::MakePointerEvent(const QPointF& item, Qt::MouseButton button,
                                Qt::KeyboardModifiers modifiers) const -> PlotPointerEvent {
  return {item, view_.ToData(item), button, modifiers};
}

auto PlotPane::IsInputLayer(const PlotLayer* layer) const -> bool {
  return layer && layer->visible() && layer->interactive();
}

void PlotPane::SetHoverLayer(PlotLayer* layer) {
  if (hover_layer_ == layer) {
    return;
  }
  if (hover_layer_) {
    hover_layer_->HoverLeave();
  }
  hover_layer_ = layer;
}

void PlotPane::UpdateCursor() {
#if QT_CONFIG(cursor)
  Qt::CursorShape shape = Qt::ArrowCursor;
  if (grab_layer_) {
    shape = grab_layer_->Cursor();
  } else if (panning_) {
    shape = Qt::ClosedHandCursor;
  } else if (hover_layer_) {
    shape = hover_layer_->Cursor();
  } else if (view_.is_zoomed()) {
    shape = Qt::OpenHandCursor;
  }
  setCursor(QCursor(shape));
#endif
}

void PlotPane::EndPointerGrab() {
  grab_layer_ = nullptr;
  panning_    = false;
  setKeepMouseGrab(false);
  UpdateCursor();
}

void PlotPane::mousePressEvent(QMouseEvent* event) {
  if (!isEnabled() || event->button() != Qt::LeftButton) {
    event->ignore();
    return;
  }
  forceActiveFocus(Qt::MouseFocusReason);
  last_pointer_item_ = event->position();
  last_modifiers_    = event->modifiers();
  const auto pointer = MakePointerEvent(event->position(), event->button(), event->modifiers());
  for (auto it = slots_.rbegin(); it != slots_.rend(); ++it) {
    PlotLayer* layer = (*it)->layer;
    if (IsInputLayer(layer) && layer->PointerPress(view_, pointer)) {
      grab_layer_ = layer;
      setKeepMouseGrab(true);
      UpdateCursor();
      event->accept();
      return;
    }
  }
  if (view_.is_zoomed() && view_.item_rect().contains(event->position())) {
    panning_ = true;
    setKeepMouseGrab(true);
    UpdateCursor();
  }
  // The press stays with the pane, so a drag on the plot does not scroll a parent Flickable.
  event->accept();
  emit backgroundPressed(pointer.data, static_cast<int>(event->modifiers()));
}

void PlotPane::mouseMoveEvent(QMouseEvent* event) {
  const QPointF position = event->position();
  last_modifiers_        = event->modifiers();
  if (grab_layer_) {
    grab_layer_->PointerMove(view_, MakePointerEvent(position, Qt::NoButton, event->modifiers()));
  } else if (panning_) {
    if (view_.Pan(position - last_pointer_item_)) {
      OnViewChanged();
    }
  }
  last_pointer_item_ = position;
  SetPointer(position, true);
  event->accept();
}

void PlotPane::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton) {
    event->ignore();
    return;
  }
  last_pointer_item_ = event->position();
  last_modifiers_    = event->modifiers();
  if (PlotLayer* layer = grab_layer_) {
    grab_layer_ = nullptr;
    layer->PointerRelease(view_,
                          MakePointerEvent(event->position(), event->button(), event->modifiers()));
  }
  EndPointerGrab();
  event->accept();
}

void PlotPane::mouseDoubleClickEvent(QMouseEvent* event) {
  if (!isEnabled() || event->button() != Qt::LeftButton) {
    event->ignore();
    return;
  }
  const auto pointer = MakePointerEvent(event->position(), event->button(), event->modifiers());
  for (auto it = slots_.rbegin(); it != slots_.rend(); ++it) {
    PlotLayer* layer = (*it)->layer;
    if (IsInputLayer(layer) && layer->PointerDoubleClick(view_, pointer)) {
      event->accept();
      return;
    }
  }
  event->ignore();
}

void PlotPane::mouseUngrabEvent() {
  // Another item took the pointer: the interaction ends at the last pointer position.
  if (PlotLayer* layer = grab_layer_) {
    grab_layer_ = nullptr;
    layer->PointerRelease(view_,
                          MakePointerEvent(last_pointer_item_, Qt::LeftButton, last_modifiers_));
  }
  EndPointerGrab();
}

void PlotPane::hoverMoveEvent(QHoverEvent* event) {
  const auto pointer = MakePointerEvent(event->position(), Qt::NoButton, event->modifiers());
  PlotLayer* taken   = nullptr;
  for (auto it = slots_.rbegin(); it != slots_.rend(); ++it) {
    PlotLayer* layer = (*it)->layer;
    if (!IsInputLayer(layer)) {
      continue;
    }
    if (!taken && layer->Hover(view_, pointer)) {
      taken = layer;
    } else if (layer != taken) {
      layer->HoverLeave();
    }
  }
  SetHoverLayer(taken);
  UpdateCursor();
  SetPointer(event->position(), true);
  event->accept();
}

void PlotPane::hoverLeaveEvent(QHoverEvent* event) {
  SetHoverLayer(nullptr);
  UpdateCursor();
  SetPointer({}, false);
  event->accept();
}

void PlotPane::wheelEvent(QWheelEvent* event) {
  if (!(event->modifiers() & Qt::ControlModifier) || event->angleDelta().y() == 0) {
    // The plain wheel scrolls the parent.
    event->ignore();
    return;
  }
  const double notches = static_cast<double>(event->angleDelta().y()) / kWheelNotch;
  const double factor  = std::pow(wheel_zoom_step_, notches);
  zoomAt(view_.ToData(event->position()), factor);
  event->accept();
}

void PlotPane::keyPressEvent(QKeyEvent* event) {
  const PlotKeyEvent key{event->key(), event->modifiers()};
  for (auto it = slots_.rbegin(); it != slots_.rend(); ++it) {
    PlotLayer* layer = (*it)->layer;
    if (IsInputLayer(layer) && layer->Key(key)) {
      event->accept();
      return;
    }
  }
  event->ignore();
}

void PlotPane::RegisterQmlTypes() {
  qmlRegisterType<PlotPane>("Alcedo.Main", 1, 0, "PlotPane");
  qmlRegisterAnonymousType<PlotLayer>("Alcedo.Main", 1);
  qmlRegisterUncreatableType<PlotHistogramSource>(
      "Alcedo.Main", 1, 0, "PlotHistogramSource",
      QStringLiteral("PlotHistogramSource is an interface of a C++ histogram owner"));
  qmlRegisterType<PlotGridLayer>("Alcedo.Main", 1, 0, "PlotGridLayer");
  qmlRegisterType<PlotHistogramLayer>("Alcedo.Main", 1, 0, "PlotHistogramLayer");
  qmlRegisterType<PlotPolylineLayer>("Alcedo.Main", 1, 0, "PlotPolylineLayer");
  qmlRegisterType<PlotHandleLayer>("Alcedo.Main", 1, 0, "PlotHandleLayer");
}

}  // namespace alcedo::ui
