//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_scope_item.hpp"

#include <qqml.h>

#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <array>
#include <cmath>

#include "ui/alcedo_main/album_backend/editor_scope_controller.hpp"

namespace alcedo::ui {
namespace {

// Fill and edge strengths of one histogram channel. The channels add up, so where all three
// overlap the plot shows a neutral tone at the same strength.
constexpr float kHistogramFillStrength = 0.36f;
constexpr float kHistogramEdgeStrength = 0.92f;
constexpr qreal kHistogramEdgeWidth    = 1.25;
// Clip marker: a corner triangle at the clipped end of the plot.
constexpr qreal kClipMarkerSize        = 7.0;
// Cut mark: a break across a peak that is higher than the plot.
constexpr qreal kPeakCutGapOffset      = 4.0;
constexpr qreal kPeakCutGapHeight      = 2.0;

auto            PlotRect(qreal width, qreal height, qreal padding) -> QRectF {
  return QRectF(padding, padding, std::max<qreal>(1.0, width - 2.0 * padding),
                           std::max<qreal>(1.0, height - 2.0 * padding));
}

auto WaveformChroma(const QColor& tint) -> std::array<float, 3> {
  const std::array<float, 3> rgb = {tint.redF(), tint.greenF(), tint.blueF()};
  const auto [minimum, maximum]  = std::minmax_element(rgb.begin(), rgb.end());
  const float range              = *maximum - *minimum;
  if (range <= 0.001f) {
    return {1.0f, 1.0f, 1.0f};
  }
  return {(rgb[0] - *minimum) / range, (rgb[1] - *minimum) / range, (rgb[2] - *minimum) / range};
}

// Chroma of the three channel tints, scaled per component so that the three add up to white.
auto AdditiveChannelChroma(const std::array<QColor, 3>& tints)
    -> std::array<std::array<float, 3>, 3> {
  std::array<std::array<float, 3>, 3> chroma = {WaveformChroma(tints[0]), WaveformChroma(tints[1]),
                                                WaveformChroma(tints[2])};
  for (size_t component = 0; component < 3; ++component) {
    const float sum = chroma[0][component] + chroma[1][component] + chroma[2][component];
    for (auto& channel : chroma) {
      channel[component] = sum > 0.001f ? channel[component] / sum : 1.0f / 3.0f;
    }
  }
  return chroma;
}

auto ScaledColor(const std::array<float, 3>& chroma, float strength) -> QColor {
  return QColor::fromRgbF(std::clamp(chroma[0] * strength, 0.0f, 1.0f),
                          std::clamp(chroma[1] * strength, 0.0f, 1.0f),
                          std::clamp(chroma[2] * strength, 0.0f, 1.0f));
}

}  // namespace

EditorScopeItem::EditorScopeItem(QQuickItem* parent) : QQuickPaintedItem(parent) {
  setAntialiasing(false);
  setOpaquePainting(true);
  setImplicitWidth(240);
  setImplicitHeight(120);
}

void EditorScopeItem::set_controller(QObject* controller) {
  if (controller_ == controller) {
    return;
  }
  auto* typed_controller = qobject_cast<EditorScopeController*>(controller);
  bind_controller(typed_controller);
  controller_ = typed_controller;
  emit ControllerChanged();
  sync_snapshot();
}

void EditorScopeItem::set_background_color(const QColor& color) {
  if (background_color_ == color) {
    return;
  }
  background_color_ = color;
  emit ColorsChanged();
  update();
}

void EditorScopeItem::set_grid_color(const QColor& color) {
  if (grid_color_ == color) {
    return;
  }
  grid_color_ = color;
  emit ColorsChanged();
  update();
}

void EditorScopeItem::set_border_color(const QColor& color) {
  if (border_color_ == color) {
    return;
  }
  border_color_ = color;
  emit ColorsChanged();
  update();
}

void EditorScopeItem::set_histogram_red_color(const QColor& color) {
  if (histogram_red_color_ == color) {
    return;
  }
  histogram_red_color_ = color;
  rebuild_waveform_image(scope_controller_ ? scope_controller_->snapshot_view().waveform
                                           : alcedo::ScopeWaveformRenderData{});
  emit ColorsChanged();
  update();
}

void EditorScopeItem::set_histogram_green_color(const QColor& color) {
  if (histogram_green_color_ == color) {
    return;
  }
  histogram_green_color_ = color;
  rebuild_waveform_image(scope_controller_ ? scope_controller_->snapshot_view().waveform
                                           : alcedo::ScopeWaveformRenderData{});
  emit ColorsChanged();
  update();
}

void EditorScopeItem::set_histogram_blue_color(const QColor& color) {
  if (histogram_blue_color_ == color) {
    return;
  }
  histogram_blue_color_ = color;
  rebuild_waveform_image(scope_controller_ ? scope_controller_->snapshot_view().waveform
                                           : alcedo::ScopeWaveformRenderData{});
  emit ColorsChanged();
  update();
}

void EditorScopeItem::set_marker_color(const QColor& color) {
  if (marker_color_ == color) {
    return;
  }
  marker_color_ = color;
  emit ColorsChanged();
  update();
}

void EditorScopeItem::set_plot_padding(qreal padding) {
  const qreal clamped = std::max<qreal>(0.0, padding);
  if (qFuzzyCompare(plot_padding_ + 1.0, clamped + 1.0)) {
    return;
  }
  plot_padding_ = clamped;
  emit PlotPaddingChanged();
  update();
}

void EditorScopeItem::RegisterQmlType() {
  qmlRegisterType<EditorScopeItem>("Alcedo.Main", 1, 0, "EditorScopeItem");
}

void EditorScopeItem::bind_controller(EditorScopeController* controller) {
  if (snapshot_connection_) {
    QObject::disconnect(snapshot_connection_);
    snapshot_connection_ = {};
  }
  if (active_view_connection_) {
    QObject::disconnect(active_view_connection_);
    active_view_connection_ = {};
  }
  if (controller_destroyed_connection_) {
    QObject::disconnect(controller_destroyed_connection_);
    controller_destroyed_connection_ = {};
  }
  scope_controller_ = controller;
  if (!scope_controller_) {
    return;
  }
  snapshot_connection_ = connect(scope_controller_, &EditorScopeController::SnapshotChanged, this,
                                 &EditorScopeItem::sync_snapshot);
  active_view_connection_ = connect(scope_controller_, &EditorScopeController::ActiveViewChanged,
                                    this, [this] { update(); });
  controller_destroyed_connection_ = connect(scope_controller_, &QObject::destroyed, this, [this] {
    controller_                      = nullptr;
    scope_controller_                = nullptr;
    snapshot_connection_             = {};
    active_view_connection_          = {};
    histogram_                       = {};
    waveform_image_                  = {};
    controller_destroyed_connection_ = {};
    update();
    emit ControllerChanged();
  });
}

void EditorScopeItem::sync_snapshot() {
  if (scope_controller_) {
    const auto& source = scope_controller_->snapshot_view();
    histogram_         = source.histogram;
    rebuild_waveform_image(source.waveform);
  } else {
    histogram_      = {};
    waveform_image_ = {};
  }
  update();
}

void EditorScopeItem::rebuild_waveform_image(const alcedo::ScopeWaveformRenderData& waveform) {
  if (!waveform.valid || waveform.width <= 0 || waveform.height <= 0 ||
      waveform.rgba.size() <
          static_cast<size_t>(waveform.width) * static_cast<size_t>(waveform.height) * 4U) {
    waveform_image_ = {};
    return;
  }

  const auto red_chroma   = WaveformChroma(histogram_red_color_);
  const auto green_chroma = WaveformChroma(histogram_green_color_);
  const auto blue_chroma  = WaveformChroma(histogram_blue_color_);

  QImage     image(waveform.width, waveform.height, QImage::Format_ARGB32_Premultiplied);
  image.fill(Qt::transparent);
  for (int y = 0; y < waveform.height; ++y) {
    auto* scanline = reinterpret_cast<QRgb*>(image.scanLine(y));
    for (int x = 0; x < waveform.width; ++x) {
      const size_t index =
          (static_cast<size_t>(y) * static_cast<size_t>(waveform.width) + static_cast<size_t>(x)) *
          4U;
      const std::array<float, 3> density = {
          std::sqrt(std::clamp(waveform.rgba[index], 0.0f, 1.0f)),
          std::sqrt(std::clamp(waveform.rgba[index + 1U], 0.0f, 1.0f)),
          std::sqrt(std::clamp(waveform.rgba[index + 2U], 0.0f, 1.0f))};
      const float peak = std::max({density[0], density[1], density[2]});
      if (peak <= 0.008f) {
        scanline[x] = qRgba(0, 0, 0, 0);
        continue;
      }

      std::array<float, 3> mixed = {};
      for (size_t component = 0; component < mixed.size(); ++component) {
        mixed[component] = red_chroma[component] * density[0] +
                           green_chroma[component] * density[1] +
                           blue_chroma[component] * density[2];
      }
      const float color_peak = std::max({mixed[0], mixed[1], mixed[2], 0.001f});
      const auto  to_byte    = [color_peak](float value) {
        return std::clamp(static_cast<int>(std::lround(value / color_peak * 255.0f)), 0, 255);
      };
      const int alpha = std::clamp(static_cast<int>(std::lround(peak * 210.0f)), 1, 210);
      scanline[x] =
          qPremultiply(qRgba(to_byte(mixed[0]), to_byte(mixed[1]), to_byte(mixed[2]), alpha));
    }
  }
  waveform_image_ = std::move(image);
}

void EditorScopeItem::paint(QPainter* painter) {
  const QRectF plot = PlotRect(width(), height(), plot_padding_);
  painter->fillRect(boundingRect(), background_color_);

  const int active_view = scope_controller_ ? scope_controller_->active_view() : 0;
  painter->setPen(QPen(grid_color_, 1.0));
  for (int i = 1; i < 4; ++i) {
    const qreal t = static_cast<qreal>(i) / 4.0;
    if (active_view == 1) {
      // Waveform: signal levels at 25, 50, and 75 percent.
      const qreal y = std::round(plot.top() + t * plot.height()) + 0.5;
      painter->drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    } else {
      // Histogram: shadow, midtone, and highlight zones. The height scale is not linear, so the
      // histogram has no level lines.
      const qreal x = std::round(plot.left() + t * plot.width()) + 0.5;
      painter->drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
    }
  }

  if (active_view == 1 && !waveform_image_.isNull()) {
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter->drawImage(plot, waveform_image_);
    painter->setRenderHint(QPainter::SmoothPixmapTransform, false);
  } else if (active_view == 0 && histogram_.valid && histogram_.bins > 1 &&
             histogram_.rgb.size() >= static_cast<size_t>(histogram_.bins) * 3U) {
    paint_histogram(painter, plot);
  }

  painter->setPen(QPen(border_color_, 1.0));
  const qreal baseline = std::round(plot.bottom()) + 0.5;
  painter->drawLine(QPointF(plot.left(), baseline), QPointF(plot.right(), baseline));

  if (active_view == 0 && histogram_.valid) {
    paint_clip_markers(painter, plot);
  }
}

void EditorScopeItem::paint_histogram(QPainter* painter, const QRectF& plot) const {
  const int  bins  = histogram_.bins;
  const auto bin_x = [&plot, bins](qreal bin) {
    return plot.left() + (bin / static_cast<qreal>(bins - 1)) * plot.width();
  };
  // A populated bin stays at least one pixel above the baseline.
  const auto bin_y = [this, &plot](size_t index) {
    const qreal value = std::clamp(static_cast<qreal>(histogram_.rgb[index]), 0.0, 1.0);
    const qreal lift  = value > 0.0 ? std::max<qreal>(1.0, value * plot.height()) : 0.0;
    return plot.bottom() - lift;
  };
  const auto chroma =
      AdditiveChannelChroma({histogram_red_color_, histogram_green_color_, histogram_blue_color_});

  painter->save();
  painter->setRenderHint(QPainter::Antialiasing, true);
  painter->setCompositionMode(QPainter::CompositionMode_Plus);
  for (size_t channel = 0; channel < 3U; ++channel) {
    const size_t offset    = channel * static_cast<size_t>(bins);
    const auto   populated = [this, offset, bins](int bin) {
      return bin >= 0 && bin < bins && histogram_.rgb[offset + static_cast<size_t>(bin)] > 0.0f;
    };
    QPainterPath area;
    // The edge leaves out empty runs, so it does not trace the baseline.
    QPainterPath edge;
    bool         edge_open = false;
    for (int bin = 0; bin < bins; ++bin) {
      const QPointF point(bin_x(bin), bin_y(offset + static_cast<size_t>(bin)));
      if (bin == 0) {
        area.moveTo(point);
      } else {
        area.lineTo(point);
      }
      if (!populated(bin - 1) && !populated(bin) && !populated(bin + 1)) {
        edge_open = false;
      } else if (!edge_open) {
        edge.moveTo(point);
        edge_open = true;
      } else {
        edge.lineTo(point);
      }
    }
    area.lineTo(plot.right(), plot.bottom());
    area.lineTo(plot.left(), plot.bottom());
    area.closeSubpath();

    painter->fillPath(area, ScaledColor(chroma[channel], kHistogramFillStrength));
    painter->strokePath(
        edge, QPen(ScaledColor(chroma[channel], kHistogramEdgeStrength), kHistogramEdgeWidth,
                   Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  }
  painter->restore();

  if (histogram_.peak_cut.size() < static_cast<size_t>(bins) * 3U) {
    return;
  }
  // A break across each run of bins whose peak is higher than the plot.
  const qreal gap_top   = plot.top() + kPeakCutGapOffset;
  int         run_start = -1;
  for (int bin = 0; bin <= bins; ++bin) {
    bool cut = false;
    for (size_t channel = 0; bin < bins && channel < 3U && !cut; ++channel) {
      cut =
          histogram_.peak_cut[channel * static_cast<size_t>(bins) + static_cast<size_t>(bin)] != 0U;
    }
    if (cut && run_start < 0) {
      run_start = bin;
    } else if (!cut && run_start >= 0) {
      const qreal left  = std::max(plot.left(), bin_x(static_cast<qreal>(run_start) - 0.5));
      const qreal right = std::min(plot.right(), bin_x(static_cast<qreal>(bin - 1) + 0.5));
      painter->fillRect(QRectF(left - 1.0, gap_top, right - left + 2.0, kPeakCutGapHeight),
                        background_color_);
      run_start = -1;
    }
  }
}

void EditorScopeItem::paint_clip_markers(QPainter* painter, const QRectF& plot) const {
  if (!histogram_.shadow_clip_warning && !histogram_.highlight_clip_warning) {
    return;
  }
  painter->save();
  painter->setRenderHint(QPainter::Antialiasing, true);
  painter->setPen(Qt::NoPen);
  painter->setBrush(marker_color_);
  if (histogram_.shadow_clip_warning) {
    const QPointF                corner = plot.topLeft();
    const std::array<QPointF, 3> points = {corner, corner + QPointF(kClipMarkerSize, 0.0),
                                           corner + QPointF(0.0, kClipMarkerSize)};
    painter->drawPolygon(points.data(), static_cast<int>(points.size()));
  }
  if (histogram_.highlight_clip_warning) {
    const QPointF                corner = plot.topRight();
    const std::array<QPointF, 3> points = {corner, corner - QPointF(kClipMarkerSize, 0.0),
                                           corner + QPointF(0.0, kClipMarkerSize)};
    painter->drawPolygon(points.data(), static_cast<int>(points.size()));
  }
  painter->restore();
}

}  // namespace alcedo::ui
