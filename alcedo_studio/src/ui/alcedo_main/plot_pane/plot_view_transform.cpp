//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/plot_pane/plot_view_transform.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace alcedo::ui {
namespace {

void ValidateRange(const PlotDataRange& range, const char* axis) {
  if (!std::isfinite(range.minimum) || !std::isfinite(range.maximum) ||
      !(range.maximum > range.minimum)) {
    throw std::invalid_argument(std::string("PlotViewTransform: the ") + axis +
                                " limits must be a finite range with maximum above minimum");
  }
}

void ValidateMaximumZoom(double maximum_zoom) {
  if (!std::isfinite(maximum_zoom) || maximum_zoom < 1.0) {
    throw std::invalid_argument("PlotViewTransform: the maximum zoom must be finite and >= 1");
  }
}

auto ScaleRangeAt(const PlotDataRange& range, double anchor, double new_span) -> PlotDataRange {
  const double span    = range.span();
  const double ratio   = span > 0.0 ? new_span / span : 1.0;
  const double minimum = anchor - (anchor - range.minimum) * ratio;
  return {minimum, minimum + new_span};
}

}  // namespace

PlotViewTransform::PlotViewTransform(const PlotDataRect& limits, double maximum_zoom) {
  ValidateMaximumZoom(maximum_zoom);
  maximum_zoom_ = maximum_zoom;
  SetLimits(limits);
}

void PlotViewTransform::SetItemRect(const QRectF& item_rect) { item_rect_ = item_rect; }

void PlotViewTransform::SetLimits(const PlotDataRect& limits) {
  ValidateRange(limits.x, "x");
  ValidateRange(limits.y, "y");
  limits_ = limits;
  view_   = limits;
}

void PlotViewTransform::SetMaximumZoom(double maximum_zoom) {
  ValidateMaximumZoom(maximum_zoom);
  maximum_zoom_ = maximum_zoom;
  SetView(view_);
}

auto PlotViewTransform::LimitedRange(const PlotDataRange& range, const PlotDataRange& limit) const
    -> PlotDataRange {
  const double limit_span = limit.span();
  const double span       = std::clamp(range.span(), limit_span / maximum_zoom_, limit_span);
  // A span that changed keeps the center of the range.
  const double center     = 0.5 * (range.minimum + range.maximum);
  double       minimum    = span == range.span() ? range.minimum : center - 0.5 * span;
  minimum                 = std::clamp(minimum, limit.minimum, limit.maximum - span);
  return {minimum, minimum + span};
}

auto PlotViewTransform::SetView(const PlotDataRect& view) -> bool {
  if (!std::isfinite(view.x.minimum) || !std::isfinite(view.x.maximum) ||
      !std::isfinite(view.y.minimum) || !std::isfinite(view.y.maximum) ||
      !(view.x.maximum > view.x.minimum) || !(view.y.maximum > view.y.minimum)) {
    return false;
  }
  const PlotDataRect limited{LimitedRange(view.x, limits_.x), LimitedRange(view.y, limits_.y)};
  if (limited == view_) {
    return false;
  }
  view_ = limited;
  return true;
}

auto PlotViewTransform::ResetView() -> bool {
  if (view_ == limits_) {
    return false;
  }
  view_ = limits_;
  return true;
}

auto PlotViewTransform::ZoomAt(const QPointF& data_anchor, double factor) -> bool {
  if (!std::isfinite(factor) || factor <= 0.0 || !std::isfinite(data_anchor.x()) ||
      !std::isfinite(data_anchor.y())) {
    return false;
  }
  // One factor for both axes: the zoom limit of either axis limits both.
  const double current_zoom = zoom();
  const double target_zoom  = std::clamp(current_zoom * factor, 1.0, maximum_zoom_);
  const double applied      = target_zoom / current_zoom;
  PlotDataRect zoomed{ScaleRangeAt(view_.x, data_anchor.x(), view_.x.span() / applied),
                      ScaleRangeAt(view_.y, data_anchor.y(), view_.y.span() / applied)};
  if (target_zoom == 1.0) {
    zoomed.x = limits_.x;
    zoomed.y = limits_.y;
  }
  return SetView(zoomed);
}

auto PlotViewTransform::Pan(const QPointF& item_delta) -> bool {
  const QPointF scale = ItemPixelsPerDataUnit();
  if (scale.x() <= 0.0 || scale.y() <= 0.0) {
    return false;
  }
  // The data follows the pointer: a pointer motion to the right shows data further left.
  const double dx = -item_delta.x() / scale.x();
  const double dy = item_delta.y() / scale.y();
  return SetView(
      {{view_.x.minimum + dx, view_.x.maximum + dx}, {view_.y.minimum + dy, view_.y.maximum + dy}});
}

auto PlotViewTransform::ToItem(const QPointF& data) const -> QPointF {
  const double tx = (data.x() - view_.x.minimum) / view_.x.span();
  const double ty = (data.y() - view_.y.minimum) / view_.y.span();
  return {item_rect_.left() + tx * item_rect_.width(),
          item_rect_.bottom() - ty * item_rect_.height()};
}

auto PlotViewTransform::ToData(const QPointF& item) const -> QPointF {
  const double width  = item_rect_.width();
  const double height = item_rect_.height();
  const double tx     = width > 0.0 ? (item.x() - item_rect_.left()) / width : 0.0;
  const double ty     = height > 0.0 ? (item_rect_.bottom() - item.y()) / height : 0.0;
  return {view_.x.minimum + tx * view_.x.span(), view_.y.minimum + ty * view_.y.span()};
}

auto PlotViewTransform::ItemPixelsPerDataUnit() const -> QPointF {
  return {item_rect_.width() / view_.x.span(), item_rect_.height() / view_.y.span()};
}

auto PlotViewTransform::zoom() const -> double { return limits_.x.span() / view_.x.span(); }

}  // namespace alcedo::ui
