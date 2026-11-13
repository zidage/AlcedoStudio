//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/plot_pane/plot_grid_layer.hpp"

#include <QVariantMap>
#include <algorithm>
#include <cmath>

#include "ui/alcedo_main/plot_pane/plot_primitives.hpp"

namespace alcedo::ui {
namespace {

auto TicksFromList(const QVariantList& list) -> std::vector<PlotTick> {
  std::vector<PlotTick> ticks;
  ticks.reserve(static_cast<size_t>(list.size()));
  for (const QVariant& entry : list) {
    const QVariantMap map   = entry.toMap();
    bool              ok    = false;
    const double      value = map.value(QStringLiteral("value")).toDouble(&ok);
    if (!ok || !std::isfinite(value)) {
      continue;
    }
    ticks.push_back({value, map.value(QStringLiteral("label")).toString(),
                     map.value(QStringLiteral("strong")).toBool()});
  }
  return ticks;
}

auto TicksToList(const std::vector<PlotTick>& ticks) -> QVariantList {
  QVariantList list;
  list.reserve(static_cast<qsizetype>(ticks.size()));
  for (const auto& tick : ticks) {
    list.push_back(QVariantMap{{QStringLiteral("value"), tick.value},
                               {QStringLiteral("label"), tick.label},
                               {QStringLiteral("strong"), tick.strong}});
  }
  return list;
}

auto InRange(double value, const PlotDataRange& range) -> bool {
  return value >= range.minimum && value <= range.maximum;
}

}  // namespace

PlotGridLayer::PlotGridLayer(QObject* parent) : PlotLayer(parent) {}

void PlotGridLayer::SetXTicks(std::vector<PlotTick> ticks) {
  if (SetContent(x_ticks_, ticks)) {
    emit ticksChanged();
  }
}

void PlotGridLayer::SetYTicks(std::vector<PlotTick> ticks) {
  if (SetContent(y_ticks_, ticks)) {
    emit ticksChanged();
  }
}

auto PlotGridLayer::x_ticks_list() const -> QVariantList { return TicksToList(x_ticks_); }

void PlotGridLayer::set_x_ticks_list(const QVariantList& ticks) { SetXTicks(TicksFromList(ticks)); }

auto PlotGridLayer::y_ticks_list() const -> QVariantList { return TicksToList(y_ticks_); }

void PlotGridLayer::set_y_ticks_list(const QVariantList& ticks) { SetYTicks(TicksFromList(ticks)); }

void PlotGridLayer::set_color(const QColor& color) {
  if (SetContent(color_, color)) {
    emit styleChanged();
  }
}

void PlotGridLayer::set_strong_color(const QColor& color) {
  if (SetContent(strong_color_, color)) {
    emit styleChanged();
  }
}

void PlotGridLayer::set_label_color(const QColor& color) {
  if (SetContent(label_color_, color)) {
    emit styleChanged();
  }
}

void PlotGridLayer::set_label_font(const QFont& font) {
  if (SetContent(label_font_, font)) {
    emit styleChanged();
  }
}

void PlotGridLayer::set_line_width(qreal width) {
  if (SetContent(line_width_, std::max<qreal>(0.0, width))) {
    emit styleChanged();
  }
}

void PlotGridLayer::set_label_gap(qreal gap) {
  if (SetContent(label_gap_, gap)) {
    emit styleChanged();
  }
}

void PlotGridLayer::AppendGeometry(const PlotViewTransform& view,
                                   PlotLayerGeometry&       geometry) const {
  const QRectF& plot   = view.item_rect();
  const auto&   range  = view.view();
  const auto    width  = static_cast<float>(line_width_);
  geometry.label_font  = label_font_;
  geometry.label_color = label_color_;

  // Ordinary lines first, so a strong line draws over a crossing ordinary line.
  for (const bool strong : {false, true}) {
    const QColor& color = strong ? strong_color_ : color_;
    for (const auto& tick : x_ticks_) {
      if (tick.strong != strong || !InRange(tick.value, range.x)) {
        continue;
      }
      const double x = view.ToItem({tick.value, range.y.minimum}).x();
      plot_primitives::AppendStroke(geometry.triangles, {{x, plot.top()}, {x, plot.bottom()}},
                                    width, color, plot);
    }
    for (const auto& tick : y_ticks_) {
      if (tick.strong != strong || !InRange(tick.value, range.y)) {
        continue;
      }
      const double y = view.ToItem({range.x.minimum, tick.value}).y();
      plot_primitives::AppendStroke(geometry.triangles, {{plot.left(), y}, {plot.right(), y}},
                                    width, color, plot);
    }
  }

  for (const auto& tick : x_ticks_) {
    if (tick.label.isEmpty() || !InRange(tick.value, range.x)) {
      continue;
    }
    const double x = view.ToItem({tick.value, range.y.minimum}).x();
    geometry.labels.push_back(
        {tick.label, {x, plot.bottom() + label_gap_}, Qt::AlignTop | Qt::AlignHCenter});
  }
  for (const auto& tick : y_ticks_) {
    if (tick.label.isEmpty() || !InRange(tick.value, range.y)) {
      continue;
    }
    const double y = view.ToItem({range.x.minimum, tick.value}).y();
    geometry.labels.push_back(
        {tick.label, {plot.left() - label_gap_, y}, Qt::AlignRight | Qt::AlignVCenter});
  }
}

}  // namespace alcedo::ui
