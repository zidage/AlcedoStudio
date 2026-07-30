//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QColor>
#include <QImage>
#include <QMetaObject>
#include <QQuickPaintedItem>

#include "edit/scope/scope_analyzer.hpp"

namespace alcedo::ui {

class EditorScopeController;

/// QPainter-backed plot for histogram and waveform snapshots.
///
/// This follows the proven QWidget scope path: convert the shared backend
/// density snapshot to one small QImage, then let Qt Quick own presentation
/// across D3D11, Metal, and OpenGL-backed windows.
class EditorScopeItem : public QQuickPaintedItem {
  Q_OBJECT
  Q_PROPERTY(QObject* controller READ controller WRITE set_controller NOTIFY ControllerChanged)
  Q_PROPERTY(
      QColor backgroundColor READ background_color WRITE set_background_color NOTIFY ColorsChanged)
  Q_PROPERTY(QColor gridColor READ grid_color WRITE set_grid_color NOTIFY ColorsChanged)
  Q_PROPERTY(QColor borderColor READ border_color WRITE set_border_color NOTIFY ColorsChanged)
  Q_PROPERTY(QColor histogramRedColor READ histogram_red_color WRITE set_histogram_red_color NOTIFY
                 ColorsChanged)
  Q_PROPERTY(QColor histogramGreenColor READ histogram_green_color WRITE set_histogram_green_color
                 NOTIFY ColorsChanged)
  Q_PROPERTY(QColor histogramBlueColor READ histogram_blue_color WRITE set_histogram_blue_color
                 NOTIFY ColorsChanged)

 public:
  explicit EditorScopeItem(QQuickItem* parent = nullptr);

  [[nodiscard]] auto controller() const -> QObject* { return controller_; }
  void               set_controller(QObject* controller);

  [[nodiscard]] auto background_color() const -> QColor { return background_color_; }
  void               set_background_color(const QColor& color);
  [[nodiscard]] auto grid_color() const -> QColor { return grid_color_; }
  void               set_grid_color(const QColor& color);
  [[nodiscard]] auto border_color() const -> QColor { return border_color_; }
  void               set_border_color(const QColor& color);
  [[nodiscard]] auto histogram_red_color() const -> QColor { return histogram_red_color_; }
  void               set_histogram_red_color(const QColor& color);
  [[nodiscard]] auto histogram_green_color() const -> QColor { return histogram_green_color_; }
  void               set_histogram_green_color(const QColor& color);
  [[nodiscard]] auto histogram_blue_color() const -> QColor { return histogram_blue_color_; }
  void               set_histogram_blue_color(const QColor& color);

  /// Register the item in the same URI used by the production QML module.
  static void        RegisterQmlType();

 signals:
  void ControllerChanged();
  void ColorsChanged();

 protected:
  void paint(QPainter* painter) override;

 private:
  void                    bind_controller(EditorScopeController* controller);
  void                    sync_snapshot();
  void                    rebuild_waveform_image(const alcedo::ScopeWaveformRenderData& waveform);

  QObject*                controller_       = nullptr;
  EditorScopeController*  scope_controller_ = nullptr;
  QMetaObject::Connection snapshot_connection_{};
  QMetaObject::Connection active_view_connection_{};
  QMetaObject::Connection controller_destroyed_connection_{};
  alcedo::ScopeHistogramRenderData histogram_{};
  QImage                           waveform_image_{};

  QColor                           background_color_      = QColor(0x16, 0x17, 0x19);
  QColor                           grid_color_            = QColor(0x3A, 0x3B, 0x3D);
  QColor                           border_color_          = QColor(0x4A, 0x4B, 0x4D);
  QColor                           histogram_red_color_   = QColor(0xE2, 0x8A, 0x8A);
  QColor                           histogram_green_color_ = QColor(0xA8, 0xD6, 0x9B);
  QColor                           histogram_blue_color_  = QColor(0x8E, 0xB9, 0xE5);
};

}  // namespace alcedo::ui
