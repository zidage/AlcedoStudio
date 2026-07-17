//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QPointF>
#include <QRectF>
#include <QTimer>
#include <QVariantAnimation>
#include <Qt>

#include <optional>

#include "ui/edit_viewer/crop_interaction_controller.hpp"
#include "ui/edit_viewer/edit_viewer_overlay_geometry.hpp"
#include "ui/edit_viewer/view_transform_controller.hpp"
#include "ui/edit_viewer/viewer_state.hpp"
#include "ui/edit_viewer/viewport_mapper.hpp"
#include "ui/viewer/viewer_view_state.hpp"

namespace alcedo::editor_rhi {

// GUI-thread owner of zoom/pan/crop interaction state for the QML editor
// viewport. Pointer handlers in QML call the typed invokables; pure geometry
// controllers do the math. Coordinates in the invokables are item/logical
// points (same space as QQuickItem mouse events), not physical pixels.
//
// Coordinate spaces (kept explicit):
// - item / logical: QML item coordinates (input, overlay placement)
// - physical pixels: item * devicePixelRatio (letterbox + render region)
// - source-image UV: normalized 0..1 over the active image / render reference
// - crop rect UV: normalized crop window in source-image UV
class EditorInteractionController : public QObject {
  Q_OBJECT
  Q_PROPERTY(float zoom READ zoom NOTIFY viewChanged)
  Q_PROPERTY(float panX READ panX NOTIFY viewChanged)
  Q_PROPERTY(float panY READ panY NOTIFY viewChanged)
  Q_PROPERTY(bool cropToolEnabled READ cropToolEnabled WRITE setCropToolEnabled NOTIFY
                 cropToolChanged)
  Q_PROPERTY(bool cropOverlayVisible READ cropOverlayVisible WRITE setCropOverlayVisible NOTIFY
                 cropChanged)
  Q_PROPERTY(QRectF cropRectNormalized READ cropRectNormalized WRITE setCropRectNormalized NOTIFY
                 cropChanged)
  Q_PROPERTY(float cropRotationDegrees READ cropRotationDegrees WRITE setCropRotationDegrees NOTIFY
                 cropChanged)
  Q_PROPERTY(bool aspectLocked READ aspectLocked NOTIFY cropChanged)
  Q_PROPERTY(float aspectRatio READ aspectRatio NOTIFY cropChanged)
  Q_PROPERTY(float metricAspect READ metricAspect NOTIFY cropChanged)
  Q_PROPERTY(int imageWidth READ imageWidth NOTIFY imageGeometryChanged)
  Q_PROPERTY(int imageHeight READ imageHeight NOTIFY imageGeometryChanged)
  Q_PROPERTY(int renderReferenceWidth READ renderReferenceWidth NOTIFY imageGeometryChanged)
  Q_PROPERTY(int renderReferenceHeight READ renderReferenceHeight NOTIFY imageGeometryChanged)
  Q_PROPERTY(qreal viewportWidth READ viewportWidth NOTIFY viewportMetricsChanged)
  Q_PROPERTY(qreal viewportHeight READ viewportHeight NOTIFY viewportMetricsChanged)
  Q_PROPERTY(qreal devicePixelRatio READ devicePixelRatio NOTIFY viewportMetricsChanged)
  Q_PROPERTY(int presentationMode READ presentationMode WRITE setPresentationMode NOTIFY
                 overlayGeometryChanged)
  Q_PROPERTY(bool detailRoiVisible READ detailRoiVisible WRITE setDetailRoiVisible NOTIFY
                 overlayGeometryChanged)
  Q_PROPERTY(QRectF detailRoiNormalized READ detailRoiNormalized WRITE setDetailRoiNormalized NOTIFY
                 overlayGeometryChanged)
  Q_PROPERTY(int cursorShape READ cursorShape NOTIFY cursorChanged)
  Q_PROPERTY(bool hasCustomCursor READ hasCustomCursor NOTIFY cursorChanged)
  Q_PROPERTY(float rotationLabelDegrees READ rotationLabelDegrees NOTIFY cropChanged)
  Q_PROPERTY(QPointF rotateHandleItemPos READ rotateHandleItemPos NOTIFY overlayGeometryChanged)
  Q_PROPERTY(bool overlayGeometryValid READ overlayGeometryValid NOTIFY overlayGeometryChanged)
  Q_PROPERTY(bool interactionEnabled READ interactionEnabled WRITE setInteractionEnabled NOTIFY
                 interactionEnabledChanged)

 public:
  explicit EditorInteractionController(QObject* parent = nullptr);

  [[nodiscard]] auto zoom() const -> float;
  [[nodiscard]] auto panX() const -> float;
  [[nodiscard]] auto panY() const -> float;
  [[nodiscard]] auto cropToolEnabled() const -> bool;
  [[nodiscard]] auto cropOverlayVisible() const -> bool;
  [[nodiscard]] auto cropRectNormalized() const -> QRectF;
  [[nodiscard]] auto cropRotationDegrees() const -> float;
  [[nodiscard]] auto aspectLocked() const -> bool;
  [[nodiscard]] auto aspectRatio() const -> float;
  [[nodiscard]] auto metricAspect() const -> float;
  [[nodiscard]] auto imageWidth() const -> int { return image_info_.image_width; }
  [[nodiscard]] auto imageHeight() const -> int { return image_info_.image_height; }
  [[nodiscard]] auto renderReferenceWidth() const -> int {
    return viewer_state_.Snapshot().render_reference_width;
  }
  [[nodiscard]] auto renderReferenceHeight() const -> int {
    return viewer_state_.Snapshot().render_reference_height;
  }
  [[nodiscard]] auto viewportWidth() const -> qreal {
    return static_cast<qreal>(widget_info_.widget_width);
  }
  [[nodiscard]] auto viewportHeight() const -> qreal {
    return static_cast<qreal>(widget_info_.widget_height);
  }
  [[nodiscard]] auto devicePixelRatio() const -> qreal {
    return static_cast<qreal>(widget_info_.device_pixel_ratio);
  }
  [[nodiscard]] auto presentationMode() const -> int {
    return static_cast<int>(presentation_mode_);
  }
  [[nodiscard]] auto detailRoiVisible() const -> bool { return detail_roi_visible_; }
  [[nodiscard]] auto detailRoiNormalized() const -> QRectF { return detail_roi_uv_; }
  [[nodiscard]] auto cursorShape() const -> int {
    return static_cast<int>(cursor_.value_or(Qt::ArrowCursor));
  }
  [[nodiscard]] auto hasCustomCursor() const -> bool { return cursor_.has_value(); }
  [[nodiscard]] auto rotationLabelDegrees() const -> float { return cropRotationDegrees(); }
  [[nodiscard]] auto rotateHandleItemPos() const -> QPointF;
  [[nodiscard]] auto overlayGeometryValid() const -> bool;
  [[nodiscard]] auto interactionEnabled() const -> bool { return interaction_enabled_; }

  void setCropToolEnabled(bool enabled);
  void setCropOverlayVisible(bool visible);
  void setCropRectNormalized(const QRectF& rect);
  void setCropRotationDegrees(float degrees);
  Q_INVOKABLE void setCropAspectLock(bool enabled, float aspect_ratio);
  void setPresentationMode(int mode);
  void setDetailRoiVisible(bool visible);
  void setDetailRoiNormalized(const QRectF& rect_uv);
  void setInteractionEnabled(bool enabled);

  // Item size is logical (QML) width/height; dpr converts to physical pixels.
  Q_INVOKABLE void setViewportMetrics(qreal width, qreal height, qreal devicePixelRatio);
  Q_INVOKABLE void setImageSize(int width, int height);
  Q_INVOKABLE void setRenderReferenceSize(int width, int height);
  // Clear crop/ROI/presentation mode and fit the view when the focused image
  // changes so A→B cannot keep A's crop/ROI overlays.
  Q_INVOKABLE void resetPresentationStateForNewImage();

  // Pointer entrypoints — positions are item/logical coordinates.
  Q_INVOKABLE void handleHoverMove(qreal x, qreal y);
  Q_INVOKABLE void handlePress(qreal x, qreal y, int button);
  Q_INVOKABLE void handleMove(qreal x, qreal y, int buttons);
  Q_INVOKABLE void handleRelease(qreal x, qreal y, int button);
  Q_INVOKABLE void handleDoubleTap(qreal x, qreal y);
  // angleDeltaY: wheel angle delta (typically ±120). pixel deltas for trackpad pan.
  // modifiers: Qt::KeyboardModifiers. synthesized=true for trackpad gestures.
  Q_INVOKABLE void handleWheel(qreal x, qreal y, int angleDeltaY, int pixelDeltaX, int pixelDeltaY,
                               int modifiers, bool synthesized);
  Q_INVOKABLE void handlePinch(qreal x, qreal y, qreal scaleDelta);
  Q_INVOKABLE void handleLeave();
  Q_INVOKABLE void resetView();
  Q_INVOKABLE void resetCropToFull();

  // Logical → source-image UV. Empty when outside the letterboxed image.
  // Explicit return types (not auto) — moc cannot parse trailing-return invokables.
  Q_INVOKABLE QPointF itemPointToImageUv(qreal x, qreal y) const;
  Q_INVOKABLE QPointF imageUvToItemPoint(qreal u, qreal v) const;
  Q_INVOKABLE bool isItemPointInsideImage(qreal x, qreal y) const;

  [[nodiscard]] auto overlaySnapshot() const -> EditViewerOverlaySnapshot;
  [[nodiscard]] auto overlayGeometry() const -> CropOverlayWidgetGeometry;
  [[nodiscard]] auto viewerViewState() const -> ViewerViewState;
  [[nodiscard]] auto viewerState() -> ViewerState& { return viewer_state_; }
  [[nodiscard]] auto viewerState() const -> const ViewerState& { return viewer_state_; }

  // Pushes the full ViewerViewState (zoom/pan, crop, viewport render region,
  // interactive/detail flags) into the production viewport + LeaseFrameSink.
  // QML must call this after view/metrics/frame changes — not only setViewTransform.
  Q_INVOKABLE void applyViewStateToViewport(QObject* viewportItem);

  // Test/harness helpers: force a view transform without animation.
  void applyViewTransformForTest(float zoom, float pan_x, float pan_y);

 signals:
  void viewChanged();
  void cropChanged();
  void cropToolChanged();
  void cropRectCommitted(const QRectF& rect, bool isFinal);
  void cropRotationCommitted(float degrees, bool isFinal);
  void overlayGeometryChanged();
  void cursorChanged();
  void imageGeometryChanged();
  void viewportMetricsChanged();
  void interactionEnabledChanged();
  void viewZoomChanged(float zoom);
  void viewStateChanged();

 private:
  static constexpr int kZoomAnimationDurationMs = 170;

  [[nodiscard]] auto widgetInfo() const -> ViewportWidgetInfo { return widget_info_; }
  [[nodiscard]] auto imageInfo() const -> ViewportImageInfo { return image_info_; }
  [[nodiscard]] auto interactionImageInfo() const -> ViewportImageInfo;
  void applyViewTransformResult(const ViewTransformResult& result);
  void applyCropInteractionResult(const CropInteractionResult& result);
  void applyCursor(std::optional<Qt::CursorShape> cursor, bool unset);
  void stopZoomAnimation();
  void emitViewAndOverlay();
  void updateViewportRenderRegionCache();
  void reconcileViewTransformForRenderReference();

  ViewerState viewer_state_{};
  ViewTransformController view_transform_controller_{};
  CropInteractionController crop_interaction_controller_{};
  ViewportWidgetInfo widget_info_{1, 1, 1.0f};
  ViewportImageInfo image_info_{0, 0};
  FramePresentationMode presentation_mode_ = FramePresentationMode::FullFrame;
  bool detail_roi_visible_ = false;
  QRectF detail_roi_uv_{0.0, 0.0, 1.0, 1.0};
  bool interaction_enabled_ = true;
  std::optional<Qt::CursorShape> cursor_{};
  QVariantAnimation* zoom_animation_ = nullptr;
  QTimer* click_toggle_timer_ = nullptr;
};

void RegisterEditorOverlayQmlTypes();

}  // namespace alcedo::editor_rhi
