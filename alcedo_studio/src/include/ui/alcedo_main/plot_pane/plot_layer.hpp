//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QColor>
#include <QFont>
#include <QObject>
#include <QPointF>
#include <QString>
#include <Qt>
#include <vector>

#include "ui/alcedo_main/plot_pane/plot_view_transform.hpp"
#include "ui/edit_viewer/mask_overlay_geometry.hpp"

namespace alcedo::ui {

/// Pointer event of a plot pane, in item coordinates and in data coordinates of the view.
struct PlotPointerEvent {
  QPointF               item{};
  QPointF               data{};
  Qt::MouseButton       button = Qt::NoButton;
  Qt::KeyboardModifiers modifiers{};
};

struct PlotKeyEvent {
  int                   key = 0;
  Qt::KeyboardModifiers modifiers{};
};

/// Text of a layer at @p anchor (item coordinates). @p alignment says which point of the text
/// box sits on the anchor.
struct PlotLabel {
  QString       text{};
  QPointF       anchor{};
  Qt::Alignment alignment = Qt::AlignCenter;
};

/**
 * @brief Output of one layer build: premultiplied triangles and labels in item coordinates.
 *
 * The pane owns one for each layer and fills it again only when the layer changed.
 */
struct PlotLayerGeometry {
  std::vector<MaskOverlayVertex> triangles{};
  std::vector<PlotLabel>         labels{};
  QFont                          label_font{};
  QColor                         label_color{};

  void                           Clear() {
    triangles.clear();
    labels.clear();
  }
};

/**
 * @brief One drawing of a plot pane, in data coordinates through the view transform of the pane.
 *
 * A layer owns its data and its colors. It emits @ref contentChanged when its drawing changes;
 * the pane then builds the layer again on the GUI thread (polish) and uploads it in the next
 * scene-graph sync. A layer that is visible and interactive receives the input that the pane
 * routes to it (top layer first). All calls run on the GUI thread.
 */
class PlotLayer : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool visible READ visible WRITE set_visible NOTIFY visibleChanged)
  Q_PROPERTY(bool interactive READ interactive WRITE set_interactive NOTIFY interactiveChanged)

 public:
  explicit PlotLayer(QObject* parent = nullptr);
  ~PlotLayer() override;

  [[nodiscard]] auto visible() const -> bool { return visible_; }
  void               set_visible(bool visible);
  [[nodiscard]] auto interactive() const -> bool { return interactive_; }
  void               set_interactive(bool interactive);

  /// Clears @p geometry and appends the drawing of the layer when it is visible.
  void               Build(const PlotViewTransform& view, PlotLayerGeometry& geometry) const;

  // Input. The pane calls these only on a visible, interactive layer.
  /// Starts an interaction. Returning true gives this layer the following moves and the release.
  virtual auto PointerPress(const PlotViewTransform& view, const PlotPointerEvent& event) -> bool;
  virtual void PointerMove(const PlotViewTransform& view, const PlotPointerEvent& event);
  /// Ends the interaction. The pane also calls it when it loses the pointer grab, with the last
  /// pointer position.
  virtual void PointerRelease(const PlotViewTransform& view, const PlotPointerEvent& event);
  virtual auto PointerDoubleClick(const PlotViewTransform& view, const PlotPointerEvent& event)
      -> bool;
  /// Returns true when the layer takes the hover at @p event. The pane then gives
  /// @ref HoverLeave to the layer that had it before.
  virtual auto Hover(const PlotViewTransform& view, const PlotPointerEvent& event) -> bool;
  virtual void HoverLeave();
  /// Cursor of the pane while this layer has the hover or the pointer grab.
  [[nodiscard]] virtual auto Cursor() const -> Qt::CursorShape;
  virtual auto               Key(const PlotKeyEvent& event) -> bool;

 signals:
  void visibleChanged();
  void interactiveChanged();
  /// The drawing changed: the pane builds this layer again.
  void contentChanged();

 protected:
  virtual void AppendGeometry(const PlotViewTransform& view, PlotLayerGeometry& geometry) const = 0;

  /// Assigns @p value to @p field and emits @ref contentChanged when they differ. Returns true
  /// when the field changed, so the caller emits the notify signal of its property.
  template <typename T>
  auto SetContent(T& field, const T& value) -> bool {
    if (field == value) {
      return false;
    }
    field = value;
    emit contentChanged();
    return true;
  }

 private:
  bool visible_     = true;
  bool interactive_ = false;
};

}  // namespace alcedo::ui
