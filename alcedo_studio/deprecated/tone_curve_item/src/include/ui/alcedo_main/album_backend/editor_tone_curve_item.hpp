//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QColor>
#include <QPointF>
#include <QQuickItem>
#include <QRectF>

#include <vector>

#include "ui/alcedo_main/album_backend/editor_tone_curve_model.hpp"

namespace alcedo::ui {

/// Pure, testable geometry description for the tone-curve editor. Vertices are
/// in item/logical coordinates. Built on the GUI thread; the QQuickItem copies
/// into scene-graph nodes at updatePaintNode time.
struct ToneCurveSceneGeometry {
  QRectF               plot_rect;
  // Line-strip samples of the Hermite curve (item space).
  std::vector<QPointF> curve_samples;
  // Grid as independent line pairs (two points per segment).
  std::vector<QPointF> grid_segments;
  // Diagonal identity reference (two points).
  std::vector<QPointF> diagonal;
  // Handle centers in item space (same order as control points).
  std::vector<QPointF> handles;
  int                  active_index = -1;
  int                  sample_count = 0;
};

/// Plot padding matches the legacy ToneCurveWidget so interaction tests can
/// share coordinate expectations with the QWidget path.
[[nodiscard]] auto ToneCurvePlotRect(qreal width, qreal height) -> QRectF;
[[nodiscard]] auto ToneCurveToWidgetPoint(const QPointF& normalized, const QRectF& plot)
    -> QPointF;
[[nodiscard]] auto ToneCurveToNormalizedPoint(const QPointF& widget_point, const QRectF& plot)
    -> QPointF;
[[nodiscard]] auto ToneCurveHitTestPoint(const QPointF&                    widget_point,
                                         const std::vector<QPointF>&       normalized_points,
                                         const QRectF&                     plot,
                                         qreal                             hit_radius = 10.0)
    -> int;
[[nodiscard]] auto BuildToneCurveSceneGeometry(const std::vector<QPointF>& normalized_points,
                                               qreal                       item_width,
                                               qreal                       item_height,
                                               int                         active_index,
                                               int sample_count = 240) -> ToneCurveSceneGeometry;

/// QQuickItem that renders the tone curve as retained QSGGeometryNode content
/// and drives `EditorToneCurveModel` through pointer input. Does not touch the
/// pipeline, journal, or render coordinator — only the model submitter seam.
class EditorToneCurveItem : public QQuickItem {
  Q_OBJECT
  Q_PROPERTY(EditorToneCurveModel* model READ model WRITE setModel NOTIFY modelChanged)
  Q_PROPERTY(QColor backgroundColor READ backgroundColor WRITE setBackgroundColor NOTIFY
                 colorsChanged)
  Q_PROPERTY(QColor plotColor READ plotColor WRITE setPlotColor NOTIFY colorsChanged)
  Q_PROPERTY(QColor gridColor READ gridColor WRITE setGridColor NOTIFY colorsChanged)
  Q_PROPERTY(QColor diagonalColor READ diagonalColor WRITE setDiagonalColor NOTIFY colorsChanged)
  Q_PROPERTY(QColor curveColor READ curveColor WRITE setCurveColor NOTIFY colorsChanged)
  Q_PROPERTY(QColor handleColor READ handleColor WRITE setHandleColor NOTIFY colorsChanged)
  Q_PROPERTY(QColor handleActiveColor READ handleActiveColor WRITE setHandleActiveColor NOTIFY
                 colorsChanged)
  Q_PROPERTY(QColor handleOutlineColor READ handleOutlineColor WRITE setHandleOutlineColor NOTIFY
                 colorsChanged)
  Q_PROPERTY(int geometryRevision READ geometryRevision NOTIFY geometryRevisionChanged)

 public:
  explicit EditorToneCurveItem(QQuickItem* parent = nullptr);

  [[nodiscard]] auto model() const -> EditorToneCurveModel* { return model_; }
  void               setModel(EditorToneCurveModel* model);

  [[nodiscard]] auto backgroundColor() const -> QColor { return background_color_; }
  void               setBackgroundColor(const QColor& c);
  [[nodiscard]] auto plotColor() const -> QColor { return plot_color_; }
  void               setPlotColor(const QColor& c);
  [[nodiscard]] auto gridColor() const -> QColor { return grid_color_; }
  void               setGridColor(const QColor& c);
  [[nodiscard]] auto diagonalColor() const -> QColor { return diagonal_color_; }
  void               setDiagonalColor(const QColor& c);
  [[nodiscard]] auto curveColor() const -> QColor { return curve_color_; }
  void               setCurveColor(const QColor& c);
  [[nodiscard]] auto handleColor() const -> QColor { return handle_color_; }
  void               setHandleColor(const QColor& c);
  [[nodiscard]] auto handleActiveColor() const -> QColor { return handle_active_color_; }
  void               setHandleActiveColor(const QColor& c);
  [[nodiscard]] auto handleOutlineColor() const -> QColor { return handle_outline_color_; }
  void               setHandleOutlineColor(const QColor& c);

  [[nodiscard]] auto geometryRevision() const -> int { return geometry_revision_; }
  /// Last built geometry after a sync (tests).
  [[nodiscard]] auto lastSceneGeometry() const -> const ToneCurveSceneGeometry& {
    return last_geometry_;
  }

  Q_INVOKABLE void refreshGeometry();

 signals:
  void modelChanged();
  void colorsChanged();
  void geometryRevisionChanged();

 protected:
  auto updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) -> QSGNode* override;
  void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;

 private:
  struct CurveRootNode;
  void bindModel(EditorToneCurveModel* model);
  void onModelPointsChanged();
  void rebuildGeometry();
  void markGeometryDirty();

  EditorToneCurveModel*  model_                = nullptr;
  ToneCurveSceneGeometry last_geometry_{};
  int                    geometry_revision_    = 0;
  bool                   geometry_dirty_       = true;
  bool                   dragging_             = false;
  QPointF                drag_origin_widget_{};
  QPointF                drag_origin_normalized_{};

  // Defaults match AppTheme monochrome slider family (no accent gold/blue).
  QColor background_color_     = QColor(0x16, 0x17, 0x19);
  QColor plot_color_           = QColor(0x11, 0x12, 0x14);
  QColor grid_color_           = QColor(0x3A, 0x3B, 0x3D);
  QColor diagonal_color_       = QColor(0xAA, 0xA5, 0x9D);
  QColor curve_color_          = QColor(0xF1, 0xEE, 0xEA);  // editorSliderHandleColor
  QColor handle_color_         = QColor(0xF1, 0xEE, 0xEA);
  QColor handle_active_color_  = QColor(0xF5, 0xF1, 0xEA);  // text / bright bone
  QColor handle_outline_color_ = QColor(0x1A, 0x1B, 0x1C);
};

}  // namespace alcedo::ui
