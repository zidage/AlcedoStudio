//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QPointF>
#include <QRectF>

/**
 * @file
 * @brief Mapping between the data rectangle of a plot and its item rectangle.
 *
 * Value types only (Qt Core geometry, no Qt Quick), so tests and layers call them directly.
 * Data coordinates have the y axis up; item coordinates have the y axis down.
 */
namespace alcedo::ui {

/// Closed interval [minimum, maximum] on one data axis.
struct PlotDataRange {
  double             minimum = 0.0;
  double             maximum = 1.0;

  [[nodiscard]] auto span() const -> double { return maximum - minimum; }
  friend auto        operator==(const PlotDataRange&, const PlotDataRange&) -> bool = default;
};

/// Data rectangle: one range for each axis.
struct PlotDataRect {
  PlotDataRange x{};
  PlotDataRange y{};

  friend auto   operator==(const PlotDataRect&, const PlotDataRect&) -> bool = default;
};

/**
 * @brief Maps the visible data rectangle (the view) to the plot rectangle of an item and back.
 *
 * The view always stays inside the limits. Its span on each axis is at most the span of the
 * limits and at least the span of the limits divided by the maximum zoom. Zoom is uniform: one
 * factor applies to both axes.
 */
class PlotViewTransform {
 public:
  static constexpr double kDefaultMaximumZoom = 32.0;

  PlotViewTransform()                         = default;
  /// @throws std::invalid_argument when a range of @p limits is empty or not finite, or when
  /// @p maximum_zoom is below 1.
  PlotViewTransform(const PlotDataRect& limits, double maximum_zoom);

  /// Plot rectangle in item coordinates. An empty rectangle maps every data point to its corner.
  void               SetItemRect(const QRectF& item_rect);
  /// Replaces the limits and the view: the view becomes the new limits.
  /// @throws std::invalid_argument as the constructor.
  void               SetLimits(const PlotDataRect& limits);
  /// @throws std::invalid_argument when @p maximum_zoom is below 1 or not finite.
  void               SetMaximumZoom(double maximum_zoom);

  /// Moves the view to @p view, limited to the span and position rules. Returns true when the
  /// view changed.
  auto               SetView(const PlotDataRect& view) -> bool;
  /// Shows the full limits. Returns true when the view changed.
  auto               ResetView() -> bool;
  /// Divides the span of the view by @p factor (above 1 zooms in) with @p data_anchor at the
  /// same item position. The span rule limits the factor, and a view that leaves the limits moves
  /// back inside them. Returns true when the view changed.
  auto               ZoomAt(const QPointF& data_anchor, double factor) -> bool;
  /// Moves the view so that the data under the pointer follows a pointer motion of
  /// @p item_delta. Returns true when the view changed.
  auto               Pan(const QPointF& item_delta) -> bool;

  [[nodiscard]] auto ToItem(const QPointF& data) const -> QPointF;
  [[nodiscard]] auto ToData(const QPointF& item) const -> QPointF;
  /// Item pixels for one data unit on each axis (both positive).
  [[nodiscard]] auto ItemPixelsPerDataUnit() const -> QPointF;

  [[nodiscard]] auto item_rect() const -> const QRectF& { return item_rect_; }
  [[nodiscard]] auto view() const -> const PlotDataRect& { return view_; }
  [[nodiscard]] auto limits() const -> const PlotDataRect& { return limits_; }
  [[nodiscard]] auto maximum_zoom() const -> double { return maximum_zoom_; }
  /// Span of the limits divided by the span of the view, on the x axis.
  [[nodiscard]] auto zoom() const -> double;
  [[nodiscard]] auto is_zoomed() const -> bool { return view_ != limits_; }

 private:
  [[nodiscard]] auto LimitedRange(const PlotDataRange& range, const PlotDataRange& limit) const
      -> PlotDataRange;

  QRectF       item_rect_{};
  PlotDataRect limits_{};
  PlotDataRect view_{};
  double       maximum_zoom_ = kDefaultMaximumZoom;
};

}  // namespace alcedo::ui
