//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/plot_pane/plot_handle_layer.hpp"

#include <algorithm>

#include "ui/alcedo_main/plot_pane/plot_primitives.hpp"

namespace alcedo::ui {
namespace {

// A hovered ring grows by this many item pixels.
constexpr qreal kHoverGrowth = 1.0;

}  // namespace

PlotHandleLayer::PlotHandleLayer(QObject* parent) : PlotLayer(parent) { set_interactive(true); }

void PlotHandleLayer::set_points(const QList<QPointF>& points) {
  if (SetContent(points_, points)) {
    emit pointsChanged();
    if (hovered_index_ >= points_.size()) {
      SetHoveredIndex(-1);
    }
  }
}

void PlotHandleLayer::set_selected_index(int index) {
  if (SetContent(selected_index_, std::max(-1, index))) {
    emit selectedIndexChanged();
  }
}

void PlotHandleLayer::set_radius(qreal radius) {
  if (SetContent(radius_, std::max<qreal>(0.0, radius))) {
    emit styleChanged();
  }
}

void PlotHandleLayer::set_ring_width(qreal width) {
  if (SetContent(ring_width_, std::max<qreal>(0.0, width))) {
    emit styleChanged();
  }
}

void PlotHandleLayer::set_outline_width(qreal width) {
  if (SetContent(outline_width_, std::max<qreal>(0.0, width))) {
    emit styleChanged();
  }
}

void PlotHandleLayer::set_color(const QColor& color) {
  if (SetContent(color_, color)) {
    emit styleChanged();
  }
}

void PlotHandleLayer::set_selected_color(const QColor& color) {
  if (SetContent(selected_color_, color)) {
    emit styleChanged();
  }
}

void PlotHandleLayer::set_outline_color(const QColor& color) {
  if (SetContent(outline_color_, color)) {
    emit styleChanged();
  }
}

void PlotHandleLayer::set_hit_radius(qreal radius) {
  const qreal limited = std::max<qreal>(0.0, radius);
  if (hit_radius_ == limited) {
    return;
  }
  hit_radius_ = limited;
  emit hitRadiusChanged();
}

void PlotHandleLayer::SetHoveredIndex(int index) {
  if (SetContent(hovered_index_, index)) {
    emit hoveredIndexChanged();
  }
}

void PlotHandleLayer::SetPressedIndex(int index) {
  if (SetContent(pressed_index_, index)) {
    emit pressedIndexChanged();
  }
}

auto PlotHandleLayer::HitTest(const PlotViewTransform& view, const QPointF& item) const -> int {
  const QRectF& plot  = view.item_rect();
  int           found = -1;
  double        best  = hit_radius_ * hit_radius_;
  for (int i = 0; i < points_.size(); ++i) {
    const QPointF center = view.ToItem(points_[i]);
    if (!plot.contains(center)) {
      continue;
    }
    const QPointF d        = item - center;
    const double  distance = QPointF::dotProduct(d, d);
    if (distance <= best) {
      best  = distance;
      found = i;
    }
  }
  return found;
}

auto PlotHandleLayer::GrabbedPosition(const PlotViewTransform& view,
                                      const PlotPointerEvent&  event) const -> QPointF {
  return view.ToData(event.item + press_offset_);
}

auto PlotHandleLayer::PointerPress(const PlotViewTransform& view, const PlotPointerEvent& event)
    -> bool {
  const int index = HitTest(view, event.item);
  if (index < 0) {
    return false;
  }
  press_offset_ = view.ToItem(points_[index]) - event.item;
  SetPressedIndex(index);
  emit pressed(index, points_[index], static_cast<int>(event.modifiers));
  return true;
}

void PlotHandleLayer::PointerMove(const PlotViewTransform& view, const PlotPointerEvent& event) {
  if (pressed_index_ < 0) {
    return;
  }
  emit moved(pressed_index_, GrabbedPosition(view, event), static_cast<int>(event.modifiers));
}

void PlotHandleLayer::PointerRelease(const PlotViewTransform& view, const PlotPointerEvent& event) {
  if (pressed_index_ < 0) {
    return;
  }
  const int index = pressed_index_;
  SetPressedIndex(-1);
  emit released(index, GrabbedPosition(view, event), static_cast<int>(event.modifiers));
}

auto PlotHandleLayer::PointerDoubleClick(const PlotViewTransform& view,
                                         const PlotPointerEvent&  event) -> bool {
  const int index = HitTest(view, event.item);
  if (index < 0) {
    return false;
  }
  emit doubleClicked(index);
  return true;
}

auto PlotHandleLayer::Hover(const PlotViewTransform& view, const PlotPointerEvent& event) -> bool {
  const int index = HitTest(view, event.item);
  SetHoveredIndex(index);
  return index >= 0;
}

void PlotHandleLayer::HoverLeave() { SetHoveredIndex(-1); }

auto PlotHandleLayer::Cursor() const -> Qt::CursorShape {
  return pressed_index_ >= 0 ? Qt::ClosedHandCursor : Qt::PointingHandCursor;
}

auto PlotHandleLayer::Key(const PlotKeyEvent& event) -> bool {
  if (selected_index_ < 0 || selected_index_ >= points_.size()) {
    return false;
  }
  switch (event.key) {
    case Qt::Key_Left:
    case Qt::Key_Right:
    case Qt::Key_Up:
    case Qt::Key_Down:
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
      emit keyPressed(selected_index_, event.key, static_cast<int>(event.modifiers));
      return true;
    default:
      return false;
  }
}

void PlotHandleLayer::AppendGeometry(const PlotViewTransform& view,
                                     PlotLayerGeometry&       geometry) const {
  const QRectF& plot = view.item_rect();
  for (int i = 0; i < points_.size(); ++i) {
    const QPointF center = view.ToItem(points_[i]);
    if (!plot.contains(center)) {
      continue;
    }
    const bool  selected = i == selected_index_;
    const qreal radius =
        radius_ + (i == hovered_index_ || i == pressed_index_ ? kHoverGrowth : 0.0);
    // The outline separates the point from the histogram and the line under it.
    if (outline_width_ > 0.0 && outline_color_.alpha() > 0) {
      plot_primitives::AppendDisc(geometry.triangles, center,
                                  static_cast<float>(radius + outline_width_), outline_color_,
                                  QRectF());
    }
    if (selected) {
      plot_primitives::AppendDisc(geometry.triangles, center, static_cast<float>(radius),
                                  selected_color_, QRectF());
    } else {
      plot_primitives::AppendRing(geometry.triangles, center, static_cast<float>(radius),
                                  static_cast<float>(ring_width_), color_, QRectF());
    }
  }
}

}  // namespace alcedo::ui
