//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QColor>
#include <QList>
#include <QPointF>
#include <optional>
#include <vector>

#include "ui/alcedo_main/plot_pane/plot_layer.hpp"

namespace alcedo::ui {

/**
 * @brief Thin antialiased line through data points, solid or dashed, clipped to the plot.
 *
 * When interactive, a press within @ref hitRadius item pixels of the line starts an interaction:
 * @ref pressed gives the nearest point of the line, @ref moved and @ref released give the position
 * of that line point as it follows the pointer (the press offset kept, so it moves 1:1 without a
 * jump), all in data coordinates.
 */
class PlotPolylineLayer : public PlotLayer {
  Q_OBJECT
  Q_PROPERTY(QList<QPointF> points READ points WRITE set_points NOTIFY pointsChanged)
  Q_PROPERTY(QColor color READ color WRITE set_color NOTIFY styleChanged)
  Q_PROPERTY(qreal width READ width WRITE set_width NOTIFY styleChanged)
  Q_PROPERTY(qreal dashLength READ dash_length WRITE set_dash_length NOTIFY styleChanged)
  Q_PROPERTY(qreal gapLength READ gap_length WRITE set_gap_length NOTIFY styleChanged)
  Q_PROPERTY(qreal hitRadius READ hit_radius WRITE set_hit_radius NOTIFY hitRadiusChanged)

 public:
  explicit PlotPolylineLayer(QObject* parent = nullptr);

  [[nodiscard]] auto points() const -> const QList<QPointF>& { return points_; }
  void               set_points(const QList<QPointF>& points);
  [[nodiscard]] auto color() const -> QColor { return color_; }
  void               set_color(const QColor& color);
  [[nodiscard]] auto width() const -> qreal { return width_; }
  void               set_width(qreal width);
  [[nodiscard]] auto dash_length() const -> qreal { return dash_length_; }
  void               set_dash_length(qreal length);
  [[nodiscard]] auto gap_length() const -> qreal { return gap_length_; }
  void               set_gap_length(qreal length);
  [[nodiscard]] auto hit_radius() const -> qreal { return hit_radius_; }
  void               set_hit_radius(qreal radius);

  /// The points in item coordinates of @p view.
  [[nodiscard]] auto ItemPoints(const PlotViewTransform& view) const -> std::vector<QPointF>;
  /// Nearest point of the line to @p item (item coordinates) when it is within the hit radius.
  [[nodiscard]] auto HitTest(const PlotViewTransform& view, const QPointF& item) const
      -> std::optional<QPointF>;

  auto PointerPress(const PlotViewTransform& view, const PlotPointerEvent& event) -> bool override;
  void PointerMove(const PlotViewTransform& view, const PlotPointerEvent& event) override;
  void PointerRelease(const PlotViewTransform& view, const PlotPointerEvent& event) override;
  auto Hover(const PlotViewTransform& view, const PlotPointerEvent& event) -> bool override;
  [[nodiscard]] auto Cursor() const -> Qt::CursorShape override;

 signals:
  void pointsChanged();
  void styleChanged();
  void hitRadiusChanged();
  void pressed(QPointF position, int modifiers);
  void moved(QPointF position, int modifiers);
  void released(QPointF position, int modifiers);

 protected:
  void AppendGeometry(const PlotViewTransform& view, PlotLayerGeometry& geometry) const override;

 private:
  QList<QPointF> points_{};
  // Defaults: AppTheme plotLineColor and plotLineWidth (dark theme).
  QColor         color_       = QColor(0xE0, 0xE0, 0xE0);
  qreal          width_       = 1.25;
  qreal          dash_length_ = 0.0;
  qreal          gap_length_  = 0.0;
  qreal          hit_radius_  = 8.0;
  bool           pressed_     = false;
  // Item offset from the pointer to the pressed point of the line.
  QPointF        press_offset_{};
};

}  // namespace alcedo::ui
