//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/plot_pane/plot_histogram_layer.hpp"

#include <algorithm>
#include <array>

#include "ui/alcedo_main/plot_pane/plot_primitives.hpp"

namespace alcedo::ui {
namespace {

constexpr qreal kPeakCutGapOffset = 4.0;
constexpr qreal kPeakCutGapHeight = 2.0;

auto            HasHistogram(const alcedo::ScopeHistogramRenderData& histogram) -> bool {
  return histogram.valid && histogram.bins > 1 &&
         histogram.rgb.size() >= static_cast<size_t>(histogram.bins) * 3U;
}

}  // namespace

PlotHistogramLayer::PlotHistogramLayer(QObject* parent) : PlotLayer(parent) {}

PlotHistogramLayer::~PlotHistogramLayer() {
  QObject::disconnect(source_changed_connection_);
  QObject::disconnect(source_destroyed_connection_);
}

void PlotHistogramLayer::set_source(PlotHistogramSource* source) {
  if (source_ == source) {
    return;
  }
  QObject::disconnect(source_changed_connection_);
  QObject::disconnect(source_destroyed_connection_);
  source_changed_connection_   = {};
  source_destroyed_connection_ = {};
  source_                      = source;
  if (source_) {
    source_changed_connection_ =
        connect(source_, &PlotHistogramSource::HistogramChanged, this, &PlotLayer::contentChanged);
    source_destroyed_connection_ = connect(source_, &QObject::destroyed, this, [this] {
      source_                      = nullptr;
      source_changed_connection_   = {};
      source_destroyed_connection_ = {};
      emit sourceChanged();
      emit contentChanged();
    });
  }
  emit sourceChanged();
  emit contentChanged();
}

void PlotHistogramLayer::set_channels(Channels channels) {
  if (SetContent(channels_, channels)) {
    emit channelsChanged();
  }
}

void PlotHistogramLayer::set_data_minimum(qreal value) {
  if (SetContent(data_minimum_, value)) {
    emit dataRangeChanged();
  }
}

void PlotHistogramLayer::set_data_maximum(qreal value) {
  if (SetContent(data_maximum_, value)) {
    emit dataRangeChanged();
  }
}

void PlotHistogramLayer::set_red_color(const QColor& color) {
  if (SetContent(red_color_, color)) {
    emit styleChanged();
  }
}

void PlotHistogramLayer::set_green_color(const QColor& color) {
  if (SetContent(green_color_, color)) {
    emit styleChanged();
  }
}

void PlotHistogramLayer::set_blue_color(const QColor& color) {
  if (SetContent(blue_color_, color)) {
    emit styleChanged();
  }
}

void PlotHistogramLayer::set_cut_color(const QColor& color) {
  if (SetContent(cut_color_, color)) {
    emit styleChanged();
  }
}

void PlotHistogramLayer::set_edge_width(qreal width) {
  if (SetContent(edge_width_, std::max<qreal>(0.0, width))) {
    emit styleChanged();
  }
}

auto PlotHistogramLayer::BinItemX(const PlotViewTransform& view, int bin, int bins) const
    -> double {
  const double t = (static_cast<double>(bin) + 0.5) / static_cast<double>(bins);
  const double x = data_minimum_ + t * (data_maximum_ - data_minimum_);
  return view.ToItem({x, view.view().y.minimum}).x();
}

void PlotHistogramLayer::AppendGeometry(const PlotViewTransform& view,
                                        PlotLayerGeometry&       geometry) const {
  if (!source_ || !(data_maximum_ > data_minimum_)) {
    return;
  }
  const auto& histogram = source_->histogram();
  if (!HasHistogram(histogram)) {
    return;
  }
  const std::array<QColor, 3> tints = {red_color_, green_color_, blue_color_};
  if (channels_ == Channels::All) {
    const auto chroma = plot_primitives::AdditiveChannelChroma(tints);
    for (int channel = 0; channel < 3; ++channel) {
      const auto& c = chroma[static_cast<size_t>(channel)];
      AppendChannel(view, histogram, channel, plot_primitives::ScaledChromaColor(c, kFillStrength),
                    plot_primitives::ScaledChromaColor(c, kEdgeStrength), geometry);
    }
  } else {
    const int  channel = static_cast<int>(channels_) - static_cast<int>(Channels::Red);
    const auto chroma  = plot_primitives::TintChroma(tints[static_cast<size_t>(channel)]);
    AppendChannel(view, histogram, channel,
                  plot_primitives::ScaledChromaColor(chroma, kFillStrength),
                  plot_primitives::ScaledChromaColor(chroma, kEdgeStrength), geometry);
  }
  AppendPeakCuts(view, histogram, geometry);
}

void PlotHistogramLayer::AppendChannel(const PlotViewTransform&                view,
                                       const alcedo::ScopeHistogramRenderData& histogram,
                                       int channel, const QColor& fill, const QColor& edge,
                                       PlotLayerGeometry& geometry) const {
  const QRectF& plot   = view.item_rect();
  const int     bins   = histogram.bins;
  const size_t  offset = static_cast<size_t>(channel) * static_cast<size_t>(bins);
  const auto    value  = [&](int bin) {
    return std::clamp(static_cast<double>(histogram.rgb[offset + static_cast<size_t>(bin)]), 0.0,
                          1.0);
  };
  // A populated bin stays at least one pixel above the baseline.
  const auto top = [&](int bin) {
    const double v    = value(bin);
    const double lift = v > 0.0 ? std::max(1.0, v * plot.height()) : 0.0;
    return plot.bottom() - lift;
  };
  const auto populated = [&](int bin) { return bin >= 0 && bin < bins && value(bin) > 0.0; };

  // Filled area: one trapezoid between the centers of two neighbor bins, clipped to the plot.
  for (int bin = 0; bin + 1 < bins; ++bin) {
    if (!populated(bin) && !populated(bin + 1)) {
      continue;
    }
    const double x0 = BinItemX(view, bin, bins);
    const double x1 = BinItemX(view, bin + 1, bins);
    if (x1 <= plot.left() || x0 >= plot.right()) {
      continue;
    }
    const double  y0 = top(bin);
    const double  y1 = top(bin + 1);
    const auto    at = [&](double x) { return y0 + (y1 - y0) * (x - x0) / (x1 - x0); };
    const double  l  = std::max(x0, plot.left());
    const double  r  = std::min(x1, plot.right());
    const QPointF tl(l, at(l));
    const QPointF tr(r, at(r));
    const QPointF br(r, plot.bottom());
    const QPointF bl(l, plot.bottom());
    for (const auto& p : {tl, tr, br, tl, br, bl}) {
      geometry.triangles.push_back(plot_primitives::AdditiveVertex(p, fill, 1.0f));
    }
  }

  // Edge line: the runs around populated bins, so the edge does not trace the baseline.
  std::vector<QPointF> run;
  const auto           flush = [&] {
    if (run.size() >= 2) {
      std::vector<MaskOverlayVertex> stroke;
      plot_primitives::AppendStroke(stroke, run, static_cast<float>(edge_width_), edge, plot);
      for (auto& vertex : stroke) {
        // The stroke is premultiplied "over"; the edge adds like the fill.
        vertex.a = 0;
      }
      geometry.triangles.insert(geometry.triangles.end(), stroke.begin(), stroke.end());
    }
    run.clear();
  };
  for (int bin = 0; bin < bins; ++bin) {
    if (!populated(bin - 1) && !populated(bin) && !populated(bin + 1)) {
      flush();
      continue;
    }
    run.emplace_back(BinItemX(view, bin, bins), top(bin));
  }
  flush();
}

void PlotHistogramLayer::AppendPeakCuts(const PlotViewTransform&                view,
                                        const alcedo::ScopeHistogramRenderData& histogram,
                                        PlotLayerGeometry&                      geometry) const {
  const int bins = histogram.bins;
  if (cut_color_.alpha() == 0 || histogram.peak_cut.size() < static_cast<size_t>(bins) * 3U) {
    return;
  }
  const QRectF& plot     = view.item_rect();
  const double  half_bin = 0.5 * (BinItemX(view, 1, bins) - BinItemX(view, 0, bins));
  const auto    cut      = [&](int bin) {
    if (bin >= bins) {
      return false;
    }
    for (int channel = 0; channel < 3; ++channel) {
      const bool drawn = channels_ == Channels::All ||
                         channel == static_cast<int>(channels_) - static_cast<int>(Channels::Red);
      if (drawn && histogram.peak_cut[static_cast<size_t>(channel) * static_cast<size_t>(bins) +
                                      static_cast<size_t>(bin)] != 0U) {
        return true;
      }
    }
    return false;
  };
  int run_start = -1;
  for (int bin = 0; bin <= bins; ++bin) {
    const bool is_cut = cut(bin);
    if (is_cut && run_start < 0) {
      run_start = bin;
    } else if (!is_cut && run_start >= 0) {
      const double left  = std::max(plot.left(), BinItemX(view, run_start, bins) - half_bin - 1.0);
      const double right = std::min(plot.right(), BinItemX(view, bin - 1, bins) + half_bin + 1.0);
      if (right > left) {
        plot_primitives::AppendRect(
            geometry.triangles,
            QRectF(left, plot.top() + kPeakCutGapOffset, right - left, kPeakCutGapHeight),
            cut_color_);
      }
      run_start = -1;
    }
  }
}

}  // namespace alcedo::ui
