//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QColor>
#include <QFont>
#include <QString>
#include <QVariantList>
#include <vector>

#include "ui/alcedo_main/plot_pane/plot_layer.hpp"

namespace alcedo::ui {

/// One grid line at @p value on its axis. A strong line uses the strong color.
struct PlotTick {
  double      value                                                = 0.0;
  QString     label                                                = {};
  bool        strong                                               = false;

  friend auto operator==(const PlotTick&, const PlotTick&) -> bool = default;
};

/**
 * @brief Grid lines across the plot rectangle and their labels.
 *
 * A tick of the x axis draws a vertical line and its label centered under the plot rectangle; a
 * tick of the y axis draws a horizontal line and its label right-aligned left of the plot
 * rectangle. Ticks outside the view draw nothing. The pane insets give the labels their room.
 *
 * QML gives the ticks as a list of objects `{value: number, label: string, strong: bool}`.
 */
class PlotGridLayer : public PlotLayer {
  Q_OBJECT
  Q_PROPERTY(QVariantList xTicks READ x_ticks_list WRITE set_x_ticks_list NOTIFY ticksChanged)
  Q_PROPERTY(QVariantList yTicks READ y_ticks_list WRITE set_y_ticks_list NOTIFY ticksChanged)
  Q_PROPERTY(QColor color READ color WRITE set_color NOTIFY styleChanged)
  Q_PROPERTY(QColor strongColor READ strong_color WRITE set_strong_color NOTIFY styleChanged)
  Q_PROPERTY(QColor labelColor READ label_color WRITE set_label_color NOTIFY styleChanged)
  Q_PROPERTY(QFont labelFont READ label_font WRITE set_label_font NOTIFY styleChanged)
  Q_PROPERTY(qreal lineWidth READ line_width WRITE set_line_width NOTIFY styleChanged)
  Q_PROPERTY(qreal labelGap READ label_gap WRITE set_label_gap NOTIFY styleChanged)

 public:
  explicit PlotGridLayer(QObject* parent = nullptr);

  void               SetXTicks(std::vector<PlotTick> ticks);
  void               SetYTicks(std::vector<PlotTick> ticks);
  [[nodiscard]] auto x_ticks() const -> const std::vector<PlotTick>& { return x_ticks_; }
  [[nodiscard]] auto y_ticks() const -> const std::vector<PlotTick>& { return y_ticks_; }

  [[nodiscard]] auto x_ticks_list() const -> QVariantList;
  void               set_x_ticks_list(const QVariantList& ticks);
  [[nodiscard]] auto y_ticks_list() const -> QVariantList;
  void               set_y_ticks_list(const QVariantList& ticks);

  [[nodiscard]] auto color() const -> QColor { return color_; }
  void               set_color(const QColor& color);
  [[nodiscard]] auto strong_color() const -> QColor { return strong_color_; }
  void               set_strong_color(const QColor& color);
  [[nodiscard]] auto label_color() const -> QColor { return label_color_; }
  void               set_label_color(const QColor& color);
  [[nodiscard]] auto label_font() const -> QFont { return label_font_; }
  void               set_label_font(const QFont& font);
  [[nodiscard]] auto line_width() const -> qreal { return line_width_; }
  void               set_line_width(qreal width);
  [[nodiscard]] auto label_gap() const -> qreal { return label_gap_; }
  void               set_label_gap(qreal gap);

 signals:
  void ticksChanged();
  void styleChanged();

 protected:
  void AppendGeometry(const PlotViewTransform& view, PlotLayerGeometry& geometry) const override;

 private:
  std::vector<PlotTick> x_ticks_{};
  std::vector<PlotTick> y_ticks_{};
  // Defaults: AppTheme plotGridColor, plotGridStrongColor, plotLabelColor (dark theme).
  QColor                color_        = QColor(0x2C, 0x2C, 0x2C);
  QColor                strong_color_ = QColor(0x4D, 0x4D, 0x4D);
  QColor                label_color_  = QColor(0x7A, 0x7A, 0x7A);
  QFont                 label_font_{};
  qreal                 line_width_ = 1.0;
  qreal                 label_gap_  = 3.0;
};

}  // namespace alcedo::ui
