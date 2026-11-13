//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QColor>
#include <QMetaObject>
#include <QObject>

#include "edit/scope/scope_analyzer.hpp"
#include "ui/alcedo_main/plot_pane/plot_layer.hpp"

namespace alcedo::ui {

/**
 * @brief Owner of a histogram that a @ref PlotHistogramLayer draws.
 *
 * The layer reads @ref histogram when it builds, on the GUI thread, and builds again after
 * @ref HistogramChanged. The owner keeps the data; the layer keeps no copy.
 */
class PlotHistogramSource : public QObject {
  Q_OBJECT

 public:
  using QObject::QObject;

  /// Display heights of the histogram; `valid` is false when there is none.
  [[nodiscard]] virtual auto histogram() const -> const alcedo::ScopeHistogramRenderData& = 0;

 signals:
  void HistogramChanged();
};

/**
 * @brief Histogram as filled areas with an edge line, behind the other layers.
 *
 * The bins cover [dataMinimum, dataMaximum] on the x axis; bin i sits at the center of its
 * interval. The height follows the plot rectangle (the display height 1 is the top), so the
 * y zoom does not change it. The channels add up: where all three overlap the area is neutral.
 * A bin above the display reference (`peak_cut`) shows a break in the cut color under the top.
 */
class PlotHistogramLayer : public PlotLayer {
  Q_OBJECT
  Q_PROPERTY(
      alcedo::ui::PlotHistogramSource* source READ source WRITE set_source NOTIFY sourceChanged)
  Q_PROPERTY(Channels channels READ channels WRITE set_channels NOTIFY channelsChanged)
  Q_PROPERTY(qreal dataMinimum READ data_minimum WRITE set_data_minimum NOTIFY dataRangeChanged)
  Q_PROPERTY(qreal dataMaximum READ data_maximum WRITE set_data_maximum NOTIFY dataRangeChanged)
  Q_PROPERTY(QColor redColor READ red_color WRITE set_red_color NOTIFY styleChanged)
  Q_PROPERTY(QColor greenColor READ green_color WRITE set_green_color NOTIFY styleChanged)
  Q_PROPERTY(QColor blueColor READ blue_color WRITE set_blue_color NOTIFY styleChanged)
  Q_PROPERTY(QColor cutColor READ cut_color WRITE set_cut_color NOTIFY styleChanged)
  Q_PROPERTY(qreal edgeWidth READ edge_width WRITE set_edge_width NOTIFY styleChanged)

 public:
  /// Channels that the layer draws: all three, or one in its own tint.
  enum class Channels { All, Red, Green, Blue };
  Q_ENUM(Channels)

  static constexpr float kFillStrength = 0.36f;
  static constexpr float kEdgeStrength = 0.92f;

  explicit PlotHistogramLayer(QObject* parent = nullptr);
  ~PlotHistogramLayer() override;

  [[nodiscard]] auto source() const -> PlotHistogramSource* { return source_; }
  void               set_source(PlotHistogramSource* source);
  [[nodiscard]] auto channels() const -> Channels { return channels_; }
  void               set_channels(Channels channels);
  [[nodiscard]] auto data_minimum() const -> qreal { return data_minimum_; }
  void               set_data_minimum(qreal value);
  [[nodiscard]] auto data_maximum() const -> qreal { return data_maximum_; }
  void               set_data_maximum(qreal value);
  [[nodiscard]] auto red_color() const -> QColor { return red_color_; }
  void               set_red_color(const QColor& color);
  [[nodiscard]] auto green_color() const -> QColor { return green_color_; }
  void               set_green_color(const QColor& color);
  [[nodiscard]] auto blue_color() const -> QColor { return blue_color_; }
  void               set_blue_color(const QColor& color);
  [[nodiscard]] auto cut_color() const -> QColor { return cut_color_; }
  void               set_cut_color(const QColor& color);
  [[nodiscard]] auto edge_width() const -> qreal { return edge_width_; }
  void               set_edge_width(qreal width);

  /// Item x of the center of @p bin of a histogram of @p bins bins.
  [[nodiscard]] auto BinItemX(const PlotViewTransform& view, int bin, int bins) const -> double;

 signals:
  void sourceChanged();
  void channelsChanged();
  void dataRangeChanged();
  void styleChanged();

 protected:
  void AppendGeometry(const PlotViewTransform& view, PlotLayerGeometry& geometry) const override;

 private:
  void                    AppendChannel(const PlotViewTransform&                view,
                                        const alcedo::ScopeHistogramRenderData& histogram, int channel,
                                        const QColor& fill, const QColor& edge, PlotLayerGeometry& geometry) const;
  void                    AppendPeakCuts(const PlotViewTransform&                view,
                                         const alcedo::ScopeHistogramRenderData& histogram,
                                         PlotLayerGeometry&                      geometry) const;

  PlotHistogramSource*    source_ = nullptr;
  QMetaObject::Connection source_changed_connection_{};
  QMetaObject::Connection source_destroyed_connection_{};
  Channels                channels_     = Channels::All;
  qreal                   data_minimum_ = 0.0;
  qreal                   data_maximum_ = 1.0;
  // Defaults: AppTheme scopeHistogram{Red,Green,Blue}Color and plotSurfaceColor (dark theme).
  QColor                  red_color_    = QColor(0xE2, 0x99, 0x99);
  QColor                  green_color_  = QColor(0xB2, 0xD8, 0xA7);
  QColor                  blue_color_   = QColor(0x9D, 0xC0, 0xE4);
  QColor                  cut_color_    = QColor(0x16, 0x16, 0x16);
  qreal                   edge_width_   = 1.25;
};

}  // namespace alcedo::ui
