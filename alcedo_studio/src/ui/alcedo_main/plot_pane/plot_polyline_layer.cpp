//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/plot_pane/plot_polyline_layer.hpp"

#include <algorithm>

#include "ui/alcedo_main/plot_pane/plot_primitives.hpp"

namespace alcedo::ui {

PlotPolylineLayer::PlotPolylineLayer(QObject* parent) : PlotLayer(parent) {}

void PlotPolylineLayer::set_points(const QList<QPointF>& points) {
  if (SetContent(points_, points)) {
    emit pointsChanged();
  }
}

void PlotPolylineLayer::set_color(const QColor& color) {
  if (SetContent(color_, color)) {
    emit styleChanged();
  }
}

void PlotPolylineLayer::set_width(qreal width) {
  if (SetContent(width_, std::max<qreal>(0.0, width))) {
    emit styleChanged();
  }
}

void PlotPolylineLayer::set_dash_length(qreal length) {
  if (SetContent(dash_length_, std::max<qreal>(0.0, length))) {
    emit styleChanged();
  }
}

void PlotPolylineLayer::set_gap_length(qreal length) {
  if (SetContent(gap_length_, std::max<qreal>(0.0, length))) {
    emit styleChanged();
  }
}

void PlotPolylineLayer::set_hit_radius(qreal radius) {
  const qreal limited = std::max<qreal>(0.0, radius);
  if (hit_radius_ == limited) {
    return;
  }
  hit_radius_ = limited;
  emit hitRadiusChanged();
}

auto PlotPolylineLayer::ItemPoints(const PlotViewTransform& view) const -> std::vector<QPointF> {
  std::vector<QPointF> item;
  item.reserve(static_cast<size_t>(points_.size()));
  for (const QPointF& point : points_) {
    item.push_back(view.ToItem(point));
  }
  return item;
}

auto PlotPolylineLayer::HitTest(const PlotViewTransform& view, const QPointF& item) const
    -> std::optional<QPointF> {
  // The line is clipped to the plot rectangle, so only a pointer inside it can hit the line.
  if (!view.item_rect().contains(item)) {
    return std::nullopt;
  }
  const auto             line = ItemPoints(view);
  std::optional<QPointF> nearest;
  double                 best = hit_radius_ * hit_radius_;
  for (size_t i = 0; i + 1 < line.size(); ++i) {
    const QPointF a         = line[i];
    const QPointF ab        = line[i + 1] - a;
    const double  length_sq = QPointF::dotProduct(ab, ab);
    const double  t =
        length_sq > 0.0 ? std::clamp(QPointF::dotProduct(item - a, ab) / length_sq, 0.0, 1.0) : 0.0;
    const QPointF on       = a + ab * t;
    const QPointF d        = item - on;
    const double  distance = QPointF::dotProduct(d, d);
    if (distance <= best) {
      best    = distance;
      nearest = on;
    }
  }
  if (!nearest) {
    return std::nullopt;
  }
  return view.ToData(*nearest);
}

auto PlotPolylineLayer::PointerPress(const PlotViewTransform& view, const PlotPointerEvent& event)
    -> bool {
  const auto hit = HitTest(view, event.item);
  if (!hit) {
    return false;
  }
  pressed_      = true;
  press_offset_ = view.ToItem(*hit) - event.item;
  emit pressed(*hit, static_cast<int>(event.modifiers));
  return true;
}

void PlotPolylineLayer::PointerMove(const PlotViewTransform& view, const PlotPointerEvent& event) {
  if (pressed_) {
    emit moved(view.ToData(event.item + press_offset_), static_cast<int>(event.modifiers));
  }
}

void PlotPolylineLayer::PointerRelease(const PlotViewTransform& view,
                                       const PlotPointerEvent&  event) {
  if (!pressed_) {
    return;
  }
  pressed_ = false;
  emit released(view.ToData(event.item + press_offset_), static_cast<int>(event.modifiers));
}

auto PlotPolylineLayer::Hover(const PlotViewTransform& view, const PlotPointerEvent& event)
    -> bool {
  return HitTest(view, event.item).has_value();
}

auto PlotPolylineLayer::Cursor() const -> Qt::CursorShape { return Qt::PointingHandCursor; }

void PlotPolylineLayer::AppendGeometry(const PlotViewTransform& view,
                                       PlotLayerGeometry&       geometry) const {
  const auto line = ItemPoints(view);
  if (line.size() < 2 || width_ <= 0.0) {
    return;
  }
  if (dash_length_ > 0.0 && gap_length_ > 0.0) {
    plot_primitives::AppendDashedStroke(geometry.triangles, line, static_cast<float>(width_),
                                        static_cast<float>(dash_length_),
                                        static_cast<float>(gap_length_), color_, view.item_rect());
  } else {
    plot_primitives::AppendStroke(geometry.triangles, line, static_cast<float>(width_), color_,
                                  view.item_rect());
  }
}

}  // namespace alcedo::ui
