//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QColor>
#include <QImage>
#include <QPointF>
#include <QQuickItem>
#include <QRectF>
#include <QString>

#include "ui/alcedo_main/album_backend/editor_cdl_trackball_model.hpp"

namespace alcedo::ui {

/// Pure geometry helpers for the CDL trackball disc (item-local coordinates).
/// Matches the legacy CdlTrackballDiscWidget padding so tests can assert
/// normalized ↔ widget round-trips without a GPU scene graph.
[[nodiscard]] auto CdlTrackballDiscRect(qreal width, qreal height) -> QRectF;
[[nodiscard]] auto CdlTrackballToWidgetPoint(const QPointF& normalized, const QRectF& disc)
    -> QPointF;
[[nodiscard]] auto CdlTrackballToNormalizedPoint(const QPointF& widget_point, const QRectF& disc)
    -> QPointF;

/// QQuickItem that paints one CDL color wheel (Lift, Gamma, or Gain) and drives
/// `EditorCdlTrackballModel` through pointer input. Three instances share one
/// model in the original triangle layout. Does not touch the pipeline.
class EditorCdlTrackballItem : public QQuickItem {
  Q_OBJECT
  Q_PROPERTY(EditorCdlTrackballModel* model READ model WRITE setModel NOTIFY modelChanged)
  Q_PROPERTY(QString wheelRole READ wheelRole WRITE setWheelRole NOTIFY wheelRoleChanged)
  Q_PROPERTY(QColor backgroundColor READ backgroundColor WRITE setBackgroundColor NOTIFY
                 colorsChanged)
  Q_PROPERTY(QColor rimColor READ rimColor WRITE setRimColor NOTIFY colorsChanged)
  Q_PROPERTY(QColor crosshairColor READ crosshairColor WRITE setCrosshairColor NOTIFY colorsChanged)
  Q_PROPERTY(QColor handleColor READ handleColor WRITE setHandleColor NOTIFY colorsChanged)
  Q_PROPERTY(QColor handleOutlineColor READ handleOutlineColor WRITE setHandleOutlineColor NOTIFY
                 colorsChanged)
  Q_PROPERTY(int geometryRevision READ geometryRevision NOTIFY geometryRevisionChanged)

 public:
  explicit EditorCdlTrackballItem(QQuickItem* parent = nullptr);

  [[nodiscard]] auto model() const -> EditorCdlTrackballModel* { return model_; }
  void setModel(EditorCdlTrackballModel* model);

  [[nodiscard]] auto wheelRole() const -> QString { return wheel_role_; }
  void setWheelRole(const QString& role);

  [[nodiscard]] auto backgroundColor() const -> QColor { return background_color_; }
  void setBackgroundColor(const QColor& c);
  [[nodiscard]] auto rimColor() const -> QColor { return rim_color_; }
  void setRimColor(const QColor& c);
  [[nodiscard]] auto crosshairColor() const -> QColor { return crosshair_color_; }
  void setCrosshairColor(const QColor& c);
  [[nodiscard]] auto handleColor() const -> QColor { return handle_color_; }
  void setHandleColor(const QColor& c);
  [[nodiscard]] auto handleOutlineColor() const -> QColor { return handle_outline_color_; }
  void setHandleOutlineColor(const QColor& c);

  [[nodiscard]] auto geometryRevision() const -> int { return geometry_revision_; }
  [[nodiscard]] auto lastDiscRect() const -> QRectF { return last_disc_rect_; }
  [[nodiscard]] auto lastHandlePoint() const -> QPointF { return last_handle_point_; }

  Q_INVOKABLE void refreshGeometry();

 signals:
  void modelChanged();
  void wheelRoleChanged();
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
  struct DiscRootNode;
  void bindModel(EditorCdlTrackballModel* model);
  void onModelWheelChanged();
  void rebuildGeometry();
  void markGeometryDirty();
  [[nodiscard]] auto currentDiscPosition() const -> QPointF;

  EditorCdlTrackballModel* model_             = nullptr;
  QString                  wheel_role_        = QStringLiteral("lift");
  QRectF                   last_disc_rect_{};
  QPointF                  last_handle_point_{};
  QImage                   wheel_cache_;
  int                      geometry_revision_ = 0;
  bool                     geometry_dirty_    = true;
  bool                     dragging_          = false;
  bool                     discMoved_         = false;

  QColor background_color_      = QColor(0x0F, 0x0F, 0x0F);
  QColor rim_color_             = QColor(0x2A, 0x2A, 0x2A);
  QColor crosshair_color_       = QColor(0xB3, 0xB3, 0xB3, 180);
  QColor handle_color_          = QColor(0xFC, 0xC7, 0x04);
  QColor handle_outline_color_  = QColor(0xF2, 0xF2, 0xF2);
};

}  // namespace alcedo::ui
