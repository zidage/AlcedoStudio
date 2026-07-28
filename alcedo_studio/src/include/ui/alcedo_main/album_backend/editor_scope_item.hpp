//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QColor>
#include <QMetaObject>
#include <QQuickItem>
#include <vector>

#include "edit/scope/scope_analyzer.hpp"

namespace alcedo::ui {

class EditorScopeController;

/// Retained scene-graph plot for the histogram and waveform snapshots.
///
/// Histogram channels are line geometry and waveform density is a colored
/// point field. No QImage or per-frame CPU image upload is used by this item.
class EditorScopeItem : public QQuickItem {
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
  auto updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) -> QSGNode* override;
  void geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;

 private:
  struct ScopeRootNode;
  void                        bind_controller(EditorScopeController* controller);
  void                        sync_snapshot();
  void                        mark_scene_dirty();

  QObject*                    controller_       = nullptr;
  EditorScopeController*      scope_controller_ = nullptr;
  QMetaObject::Connection     snapshot_connection_{};
  QMetaObject::Connection     active_view_connection_{};
  QMetaObject::Connection     controller_destroyed_connection_{};
  alcedo::ScopeRenderSnapshot snapshot_{};
  bool                        scene_dirty_           = true;

  QColor                      background_color_      = QColor(0x16, 0x17, 0x19);
  QColor                      grid_color_            = QColor(0x3A, 0x3B, 0x3D);
  QColor                      border_color_          = QColor(0x4A, 0x4B, 0x4D);
  QColor                      histogram_red_color_   = QColor(0xE2, 0x8A, 0x8A);
  QColor                      histogram_green_color_ = QColor(0xA8, 0xD6, 0x9B);
  QColor                      histogram_blue_color_  = QColor(0x8E, 0xB9, 0xE5);
};

}  // namespace alcedo::ui
