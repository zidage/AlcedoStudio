//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/plot_pane/plot_layer.hpp"

namespace alcedo::ui {

PlotLayer::PlotLayer(QObject* parent) : QObject(parent) {}

PlotLayer::~PlotLayer() = default;

void PlotLayer::set_visible(bool visible) {
  if (visible_ == visible) {
    return;
  }
  visible_ = visible;
  emit visibleChanged();
  emit contentChanged();
}

void PlotLayer::set_interactive(bool interactive) {
  if (interactive_ == interactive) {
    return;
  }
  interactive_ = interactive;
  emit interactiveChanged();
}

void PlotLayer::Build(const PlotViewTransform& view, PlotLayerGeometry& geometry) const {
  geometry.Clear();
  if (visible_) {
    AppendGeometry(view, geometry);
  }
}

auto PlotLayer::PointerPress(const PlotViewTransform&, const PlotPointerEvent&) -> bool {
  return false;
}

void PlotLayer::PointerMove(const PlotViewTransform&, const PlotPointerEvent&) {}

void PlotLayer::PointerRelease(const PlotViewTransform&, const PlotPointerEvent&) {}

auto PlotLayer::PointerDoubleClick(const PlotViewTransform&, const PlotPointerEvent&) -> bool {
  return false;
}

auto PlotLayer::Hover(const PlotViewTransform&, const PlotPointerEvent&) -> bool { return false; }

void PlotLayer::HoverLeave() {}

auto PlotLayer::Cursor() const -> Qt::CursorShape { return Qt::ArrowCursor; }

auto PlotLayer::Key(const PlotKeyEvent&) -> bool { return false; }

}  // namespace alcedo::ui
