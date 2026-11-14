//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QColor>
#include <QMetaObject>
#include <QPointF>
#include <QQmlListProperty>
#include <QQuickItem>
#include <QRectF>
#include <QTextLayout>
#include <memory>
#include <vector>

#include "ui/alcedo_main/plot_pane/plot_layer.hpp"
#include "ui/alcedo_main/plot_pane/plot_view_transform.hpp"

namespace alcedo::ui {

/**
 * @brief Plot item: a view transform, a plot inset, and an ordered list of layers.
 *
 * The plot rectangle is the item rectangle minus the insets; the insets hold the labels of the
 * layers. The first layer draws at the bottom and the last on top. The view transform maps the
 * visible data rectangle (the view, inside the limits) to the plot rectangle.
 *
 * Drawing: a layer that emits `contentChanged`, a view change, and a size change mark layers
 * dirty. The pane builds the dirty layers in `updatePolish` (GUI thread) and uploads only those
 * in `updatePaintNode`.
 *
 * Input: pointer and key events go to the top visible, interactive layer that accepts them, in
 * data coordinates. The layer that accepts a press keeps the pointer grab until the release. A
 * press that no layer accepts emits @ref backgroundPressed and pans a zoomed view. Ctrl + wheel
 * zooms at the pointer; a plain wheel event stays unaccepted, so a parent Flickable scrolls.
 * @ref pointerInside and @ref pointerPosition follow the pointer over the plot rectangle (hover,
 * and the moves of a press).
 *
 * Each pane owns its view transform and its layer list, so panes in one window are independent.
 * A layer belongs to one pane.
 */
class PlotPane : public QQuickItem {
  Q_OBJECT
  Q_PROPERTY(QQmlListProperty<alcedo::ui::PlotLayer> layers READ layers_property)
  Q_CLASSINFO("DefaultProperty", "layers")
  // Data rectangle: x and width on the x axis, y and height on the y axis (y up).
  Q_PROPERTY(QRectF limits READ limits WRITE set_limits NOTIFY limitsChanged)
  Q_PROPERTY(qreal maximumZoom READ maximum_zoom WRITE set_maximum_zoom NOTIFY limitsChanged)
  Q_PROPERTY(QRectF view READ view NOTIFY viewChanged)
  Q_PROPERTY(bool zoomed READ zoomed NOTIFY viewChanged)
  Q_PROPERTY(qreal zoom READ zoom NOTIFY viewChanged)
  Q_PROPERTY(QRectF plotRect READ plot_rect NOTIFY plotRectChanged)
  Q_PROPERTY(qreal insetLeft READ inset_left WRITE set_inset_left NOTIFY insetsChanged)
  Q_PROPERTY(qreal insetTop READ inset_top WRITE set_inset_top NOTIFY insetsChanged)
  Q_PROPERTY(qreal insetRight READ inset_right WRITE set_inset_right NOTIFY insetsChanged)
  Q_PROPERTY(qreal insetBottom READ inset_bottom WRITE set_inset_bottom NOTIFY insetsChanged)
  Q_PROPERTY(
      QColor backgroundColor READ background_color WRITE set_background_color NOTIFY colorsChanged)
  Q_PROPERTY(QColor surfaceColor READ surface_color WRITE set_surface_color NOTIFY colorsChanged)
  Q_PROPERTY(qreal wheelZoomStep READ wheel_zoom_step WRITE set_wheel_zoom_step NOTIFY
                 wheelZoomStepChanged)
  /// True while the pointer is over the plot rectangle.
  Q_PROPERTY(bool pointerInside READ pointer_inside NOTIFY pointerChanged)
  /// Data position of the pointer; valid while @ref pointerInside is true.
  Q_PROPERTY(QPointF pointerPosition READ pointer_position NOTIFY pointerChanged)

 public:
  explicit PlotPane(QQuickItem* parent = nullptr);
  ~PlotPane() override;

  // Layers.
  /// Appends @p layer on top. Precondition: no other pane holds @p layer.
  void                AddLayer(PlotLayer* layer);
  void                ClearLayers();
  [[nodiscard]] auto  layer_count() const -> int { return static_cast<int>(slots_.size()); }
  [[nodiscard]] auto  layer_at(int index) const -> PlotLayer*;
  [[nodiscard]] auto  layers_property() -> QQmlListProperty<PlotLayer>;

  // View.
  [[nodiscard]] auto  view_transform() const -> const PlotViewTransform& { return view_; }
  [[nodiscard]] auto  limits() const -> QRectF;
  void                set_limits(const QRectF& limits);
  [[nodiscard]] auto  maximum_zoom() const -> qreal { return view_.maximum_zoom(); }
  void                set_maximum_zoom(qreal maximum_zoom);
  [[nodiscard]] auto  view() const -> QRectF;
  [[nodiscard]] auto  zoomed() const -> bool { return view_.is_zoomed(); }
  [[nodiscard]] auto  zoom() const -> qreal { return view_.zoom(); }
  [[nodiscard]] auto  plot_rect() const -> QRectF { return view_.item_rect(); }

  Q_INVOKABLE void    setView(const QRectF& view);
  Q_INVOKABLE void    resetView();
  /// Divides the view span by @p factor with @p data_anchor at the same item position.
  Q_INVOKABLE void    zoomAt(const QPointF& data_anchor, qreal factor);
  Q_INVOKABLE QPointF mapToData(const QPointF& item) const;
  Q_INVOKABLE QPointF mapFromData(const QPointF& data) const;

  // Style.
  [[nodiscard]] auto  inset_left() const -> qreal { return inset_left_; }
  void                set_inset_left(qreal inset);
  [[nodiscard]] auto  inset_top() const -> qreal { return inset_top_; }
  void                set_inset_top(qreal inset);
  [[nodiscard]] auto  inset_right() const -> qreal { return inset_right_; }
  void                set_inset_right(qreal inset);
  [[nodiscard]] auto  inset_bottom() const -> qreal { return inset_bottom_; }
  void                set_inset_bottom(qreal inset);
  [[nodiscard]] auto  background_color() const -> QColor { return background_color_; }
  void                set_background_color(const QColor& color);
  [[nodiscard]] auto  surface_color() const -> QColor { return surface_color_; }
  void                set_surface_color(const QColor& color);
  [[nodiscard]] auto  wheel_zoom_step() const -> qreal { return wheel_zoom_step_; }
  void                set_wheel_zoom_step(qreal step);

  // Pointer.
  [[nodiscard]] auto  pointer_inside() const -> bool { return pointer_inside_; }
  [[nodiscard]] auto  pointer_position() const -> QPointF;

  /// Builds the layers that changed since their last build. `updatePolish` calls it; tests call
  /// it without a window.
  void                BuildDirtyLayers();
  /// Number of builds of @p layer, or -1 when the pane does not hold it.
  [[nodiscard]] auto  LayerBuildCount(const PlotLayer* layer) const -> int;
  /// Last built geometry of @p layer, or nullptr when the pane does not hold it.
  [[nodiscard]] auto  LayerGeometry(const PlotLayer* layer) const -> const PlotLayerGeometry*;

  static void         RegisterQmlTypes();

 signals:
  void limitsChanged();
  void viewChanged();
  void plotRectChanged();
  void insetsChanged();
  void colorsChanged();
  void wheelZoomStepChanged();
  void pointerChanged();
  /// A press of the left button that no layer accepted, at @p position (data coordinates).
  void backgroundPressed(QPointF position, int modifiers);

 protected:
  void updatePolish() override;
  auto updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) -> QSGNode* override;
  void geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;
  void mouseUngrabEvent() override;
  void hoverMoveEvent(QHoverEvent* event) override;
  void hoverLeaveEvent(QHoverEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;

 private:
  struct LayerSlot {
    PlotLayer*                                layer = nullptr;
    PlotLayerGeometry                         geometry{};
    // Text layouts of geometry.labels and their top-left positions, built with the geometry.
    std::vector<std::unique_ptr<QTextLayout>> text_layouts{};
    std::vector<QPointF>                      text_positions{};
    QMetaObject::Connection                   content_connection{};
    QMetaObject::Connection                   destroyed_connection{};
    bool                                      dirty        = true;
    bool                                      needs_upload = true;
    int                                       build_count  = 0;
  };
  struct PaneRootNode;

  [[nodiscard]] auto FindSlot(const PlotLayer* layer) const -> LayerSlot*;
  void               RemoveSlot(const PlotLayer* layer);
  void               MarkLayerDirty(LayerSlot& slot);
  void               MarkAllLayersDirty();
  void               UpdatePlotRect();
  void               OnViewChanged();
  void               BuildLabels(LayerSlot& slot) const;
  [[nodiscard]] auto MakePointerEvent(const QPointF& item, Qt::MouseButton button,
                                      Qt::KeyboardModifiers modifiers) const -> PlotPointerEvent;
  [[nodiscard]] auto IsInputLayer(const PlotLayer* layer) const -> bool;
  void               SetHoverLayer(PlotLayer* layer);
  void               UpdateCursor();
  void               EndPointerGrab();
  /// Sets the pointer to @p item (item coordinates), or to outside the plot when @p inside is
  /// false.
  void               SetPointer(const QPointF& item, bool inside);

  static void        AppendLayer(QQmlListProperty<PlotLayer>* list, PlotLayer* layer);
  static auto        LayerCount(QQmlListProperty<PlotLayer>* list) -> qsizetype;
  static auto        LayerAt(QQmlListProperty<PlotLayer>* list, qsizetype index) -> PlotLayer*;
  static void        ClearLayerList(QQmlListProperty<PlotLayer>* list);

  PlotViewTransform  view_{};
  std::vector<std::unique_ptr<LayerSlot>> slots_{};
  bool                                    layer_list_changed_ = true;
  bool                                    base_changed_       = true;

  PlotLayer*                              grab_layer_         = nullptr;
  PlotLayer*                              hover_layer_        = nullptr;
  bool                                    panning_            = false;
  QPointF                                 last_pointer_item_{};
  Qt::KeyboardModifiers                   last_modifiers_{};

  qreal                                   inset_left_       = 0.0;
  qreal                                   inset_top_        = 0.0;
  qreal                                   inset_right_      = 0.0;
  qreal                                   inset_bottom_     = 0.0;
  QColor                                  background_color_ = QColor(Qt::transparent);
  // Default: AppTheme plotSurfaceColor (dark theme).
  QColor                                  surface_color_    = QColor(0x16, 0x16, 0x16);
  qreal                                   wheel_zoom_step_  = 1.25;
  // Item position of the pointer; the data position follows the view.
  QPointF                                 pointer_item_{};
  bool                                    pointer_inside_ = false;
};

}  // namespace alcedo::ui
