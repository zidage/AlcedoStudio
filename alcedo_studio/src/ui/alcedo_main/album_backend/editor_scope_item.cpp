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

auto PlotRect(qreal width, qreal height) -> QRectF {
  constexpr qreal kInset = 1.0;
  return QRectF(kInset, kInset, std::max<qreal>(1.0, width - 2.0 * kInset),
                std::max<qreal>(1.0, height - 2.0 * kInset));
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

}  // namespace

EditorScopeItem::EditorScopeItem(QQuickItem* parent) : QQuickPaintedItem(parent) {
  setAntialiasing(false);
  setOpaquePainting(true);
  setTextureSize(QSize(320, 160));
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
  const QRectF plot = PlotRect(width(), height());
  painter->fillRect(boundingRect(), background_color_);

  const int active_view = scope_controller_ ? scope_controller_->active_view() : 0;
  if (active_view == 1 && !waveform_image_.isNull()) {
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter->drawImage(plot, waveform_image_);
    painter->setRenderHint(QPainter::SmoothPixmapTransform, false);
  } else if (active_view == 0 && histogram_.valid && histogram_.bins > 1 &&
             histogram_.rgb.size() >= static_cast<size_t>(histogram_.bins) * 3U) {
    painter->setRenderHint(QPainter::Antialiasing, true);
    const std::array<QColor, 3> colors = {histogram_red_color_, histogram_green_color_,
                                          histogram_blue_color_};
    for (int channel = 0; channel < 3; ++channel) {
      QPainterPath path;
      for (int bin = 0; bin < histogram_.bins; ++bin) {
        const float value = std::clamp(
            histogram_.rgb[static_cast<size_t>(channel * histogram_.bins + bin)], 0.0f, 1.0f);
        const qreal x =
            plot.left() +
            (static_cast<qreal>(bin) / static_cast<qreal>(histogram_.bins - 1)) * plot.width();
        const qreal y = plot.bottom() - static_cast<qreal>(value) * plot.height();
        if (bin == 0) {
          path.moveTo(x, y);
        } else {
          path.lineTo(x, y);
        }
      }
      painter->setPen(QPen(colors[static_cast<size_t>(channel)], 1.0));
      painter->drawPath(path);
    }
    painter->setRenderHint(QPainter::Antialiasing, false);
  }

  painter->setPen(QPen(grid_color_, 1.0));
  for (int i = 1; i < 4; ++i) {
    const qreal t = static_cast<qreal>(i) / 4.0;
    const qreal y = plot.top() + t * plot.height();
    painter->drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    if (active_view == 0) {
      const qreal x = plot.left() + t * plot.width();
      painter->drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
    }
  }

  painter->setPen(QPen(border_color_, 1.0));
  painter->drawLine(plot.topLeft(), plot.topRight());
  painter->drawLine(plot.bottomLeft(), plot.bottomRight());
}

}  // namespace alcedo::ui
