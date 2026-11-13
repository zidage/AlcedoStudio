//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QColor>
#include <QList>
#include <QPointF>

#include "ui/alcedo_main/plot_pane/plot_layer.hpp"

namespace alcedo::ui {

/**
 * @brief Control points drawn as rings; the selected point is a filled disc.
 *
 * The layer draws and hit-tests the points; its owner keeps them. A press within
 * @ref hitRadius item pixels of a point grabs it: @ref pressed, then @ref moved for each pointer
 * motion, then @ref released. The positions are data coordinates of the point under the pointer,
 * with the press offset kept, so the point follows the pointer 1:1 without a jump. The owner
 * moves the point by setting @ref points. With a selected point, the arrow keys, Delete, and
 * Backspace give @ref keyPressed.
 */
class PlotHandleLayer : public PlotLayer {
  Q_OBJECT
  Q_PROPERTY(QList<QPointF> points READ points WRITE set_points NOTIFY pointsChanged)
  Q_PROPERTY(
      int selectedIndex READ selected_index WRITE set_selected_index NOTIFY selectedIndexChanged)
  Q_PROPERTY(int hoveredIndex READ hovered_index NOTIFY hoveredIndexChanged)
  Q_PROPERTY(int pressedIndex READ pressed_index NOTIFY pressedIndexChanged)
  Q_PROPERTY(qreal radius READ radius WRITE set_radius NOTIFY styleChanged)
  Q_PROPERTY(qreal ringWidth READ ring_width WRITE set_ring_width NOTIFY styleChanged)
  Q_PROPERTY(qreal outlineWidth READ outline_width WRITE set_outline_width NOTIFY styleChanged)
  Q_PROPERTY(QColor color READ color WRITE set_color NOTIFY styleChanged)
  Q_PROPERTY(QColor selectedColor READ selected_color WRITE set_selected_color NOTIFY styleChanged)
  Q_PROPERTY(QColor outlineColor READ outline_color WRITE set_outline_color NOTIFY styleChanged)
  Q_PROPERTY(qreal hitRadius READ hit_radius WRITE set_hit_radius NOTIFY hitRadiusChanged)

 public:
  explicit PlotHandleLayer(QObject* parent = nullptr);

  [[nodiscard]] auto points() const -> const QList<QPointF>& { return points_; }
  void               set_points(const QList<QPointF>& points);
  [[nodiscard]] auto selected_index() const -> int { return selected_index_; }
  void               set_selected_index(int index);
  [[nodiscard]] auto hovered_index() const -> int { return hovered_index_; }
  [[nodiscard]] auto pressed_index() const -> int { return pressed_index_; }
  [[nodiscard]] auto radius() const -> qreal { return radius_; }
  void               set_radius(qreal radius);
  [[nodiscard]] auto ring_width() const -> qreal { return ring_width_; }
  void               set_ring_width(qreal width);
  [[nodiscard]] auto outline_width() const -> qreal { return outline_width_; }
  void               set_outline_width(qreal width);
  [[nodiscard]] auto color() const -> QColor { return color_; }
  void               set_color(const QColor& color);
  [[nodiscard]] auto selected_color() const -> QColor { return selected_color_; }
  void               set_selected_color(const QColor& color);
  [[nodiscard]] auto outline_color() const -> QColor { return outline_color_; }
  void               set_outline_color(const QColor& color);
  [[nodiscard]] auto hit_radius() const -> qreal { return hit_radius_; }
  void               set_hit_radius(qreal radius);

  /// Index of the point nearest to @p item within the hit radius (item pixels), or -1. A point
  /// outside the plot rectangle is not drawn and cannot be hit.
  [[nodiscard]] auto HitTest(const PlotViewTransform& view, const QPointF& item) const -> int;

  auto PointerPress(const PlotViewTransform& view, const PlotPointerEvent& event) -> bool override;
  void PointerMove(const PlotViewTransform& view, const PlotPointerEvent& event) override;
  void PointerRelease(const PlotViewTransform& view, const PlotPointerEvent& event) override;
  auto PointerDoubleClick(const PlotViewTransform& view, const PlotPointerEvent& event)
      -> bool override;
  auto Hover(const PlotViewTransform& view, const PlotPointerEvent& event) -> bool override;
  void HoverLeave() override;
  [[nodiscard]] auto Cursor() const -> Qt::CursorShape override;
  auto               Key(const PlotKeyEvent& event) -> bool override;

 signals:
  void pointsChanged();
  void selectedIndexChanged();
  void hoveredIndexChanged();
  void pressedIndexChanged();
  void styleChanged();
  void hitRadiusChanged();
  void pressed(int index, QPointF position, int modifiers);
  void moved(int index, QPointF position, int modifiers);
  void released(int index, QPointF position, int modifiers);
  void doubleClicked(int index);
  void keyPressed(int index, int key, int modifiers);

 protected:
  void AppendGeometry(const PlotViewTransform& view, PlotLayerGeometry& geometry) const override;

 private:
  void               SetHoveredIndex(int index);
  void               SetPressedIndex(int index);
  [[nodiscard]] auto GrabbedPosition(const PlotViewTransform& view,
                                     const PlotPointerEvent&  event) const -> QPointF;

  QList<QPointF>     points_{};
  int                selected_index_ = -1;
  int                hovered_index_  = -1;
  int                pressed_index_  = -1;
  // Item offset from the pointer to the center of the pressed point.
  QPointF            press_offset_{};
  // Defaults: AppTheme plotHandleRadius, plotHandleRingWidth, plotHandleColor,
  // plotHandleOutlineColor, and plotHitRadius (dark theme).
  qreal              radius_         = 3.5;
  qreal              ring_width_     = 1.25;
  qreal              outline_width_  = 1.0;
  QColor             color_          = QColor(0xE0, 0xE0, 0xE0);
  QColor             selected_color_ = QColor(0xE0, 0xE0, 0xE0);
  QColor             outline_color_  = QColor(0x1A, 0x1A, 0x1A, 0xC8);
  qreal              hit_radius_     = 8.0;
};

}  // namespace alcedo::ui
