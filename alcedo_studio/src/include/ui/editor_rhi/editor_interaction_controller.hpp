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
  // True zoom = physical screen pixels per source-image pixel (1.0 = 1:1 = 100%),
  // derived from the full image size, viewport metrics and the fit-relative zoom
  // field. Independent of the 2K preview cap — that cap only selects the render
  // tier (full-frame base vs. DetailPatch), not the displayed ratio. See trueZoom().
  Q_PROPERTY(float trueZoom READ trueZoom NOTIFY zoomLabelChanged)
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
  // Phase 5D: view-change kind reported to the session controller for render
  // routing. UI-level only — the album backend maps this to an EditorRenderReason
  // and the coordinator decides reuse vs. InteractivePrimary vs. DetailPatch.
  // Input handlers report the new view; they never choose or submit pipeline
  // tasks (plan Phase 5D D2).
  enum class ViewChangeKind {
    ZoomPan,        // zoom/pan transform (reuse or detail, decided downstream)
    Resize,         // viewport metrics (size/dpr) changed
    CropRotate,     // crop rect or rotation changed (content change)
    DetailRefresh,  // ROI detail region/mode changed
  };
  Q_ENUM(ViewChangeKind)

  explicit EditorInteractionController(QObject* parent = nullptr);

  [[nodiscard]] auto zoom() const -> float;
  // True zoom ratio (1.0 = 1:1). Returns 0 when no image / viewport is set.
  [[nodiscard]] auto trueZoom() const -> float;
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

  Q_INVOKABLE void setCropToolEnabled(bool enabled);
  Q_INVOKABLE void setCropOverlayVisible(bool visible);
  Q_INVOKABLE void setCropRectNormalized(const QRectF& rect);
  Q_INVOKABLE void setCropRotationDegrees(float degrees);
  Q_INVOKABLE void setCropAspectLock(bool enabled, float aspect_ratio);
  // When false, crop/rotation/view setters still update state and overlay
  // signals but do not emit viewChangeReported. Geometry panel enter/leave
  // uses this so EditorSessionController owns the single source-frame refresh.
  Q_INVOKABLE void setViewChangeRoutingEnabled(bool enabled);
  void setPresentationMode(int mode);
  void setDetailRoiVisible(bool visible);
  void setDetailRoiNormalized(const QRectF& rect_uv);
  void setInteractionEnabled(bool enabled);

  // Item size is logical (QML) width/height; dpr converts to physical pixels.
  Q_INVOKABLE void setViewportMetrics(qreal width, qreal height, qreal devicePixelRatio);
  Q_INVOKABLE void setImageSize(int width, int height);
  Q_INVOKABLE void setRenderReferenceSize(int width, int height);
  /// Force-apply render-reference size even when width/height match the previous
  /// values. Used when a new image/session generation reuses the same output size
  /// (Phase 5B equal-output-size geometry sync).
  Q_INVOKABLE void forceRenderReferenceSize(int width, int height);
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
  // modifiers: Qt::KeyboardModifiers. synthesized=true for trackpad input.
  Q_INVOKABLE void handleWheel(qreal x, qreal y, int angleDeltaY, int pixelDeltaX, int pixelDeltaY,
                               int modifiers, bool synthesized);
  // Relative pinch step (ZoomNativeGesture-style): zoom *= (1 + scaleDelta).
  // Prefer handlePinchTo for Qt Quick PinchHandler.
  Q_INVOKABLE void handlePinch(qreal x, qreal y, qreal scaleDelta);
  // Absolute pinch target zoom (fit-relative field). QML PinchHandler should
  // compute startZoom * (scale / startScale) and call this each scale update.
  Q_INVOKABLE void handlePinchTo(qreal x, qreal y, qreal targetZoom);
  // Marks the lifetime of a Qt Quick wheel/pinch input sequence. While active,
  // view changes only re-sample the current base; one final DetailRefresh is
  // emitted when the sequence ends.
  Q_INVOKABLE void beginViewInputSequence();
  Q_INVOKABLE void finishViewInputSequence();
  Q_INVOKABLE void handleLeave();
  Q_INVOKABLE void resetView();
  // Snap to 1:1 (true zoom = 1.0, one image pixel per screen pixel), centered.
  // For images whose fit already exceeds 100% (small images / large viewports),
  // this clamps to fit since 1:1 lies below the fit floor.
  Q_INVOKABLE void zoomToActualPixels();
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
  // interactive/detail flags) into the production viewport + DirectFrameSink.
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
  // Fires when the displayed zoom ratio (trueZoom) may have changed: zoom field
  // change, image size change, or viewport metrics change.
  void zoomLabelChanged();
  void viewStateChanged();
  // Phase 5D: a user-driven view change occurred (zoom/pan/resize/crop-
  // rotation/ROI). Carries a ViewChangeKind (int for QML). Emitted AFTER
  // viewStateChanged so the QML push of the new view to the viewport (and its
  // sink region) lands before the session routes the render intent.
  void viewChangeReported(int kind);

 private:
  static constexpr int kZoomAnimationDurationMs = 170;
  static constexpr int kViewInteractionSettleDelayMs = 120;
  // True-zoom ceiling (1600%, matching professional editors). The zoom *field*
  // ceiling is derived from this and the current fit fraction so the cap is
  // consistent across image sizes and viewport DPIs.
  static constexpr float kMaxTrueZoom = 16.0f;

  [[nodiscard]] auto widgetInfo() const -> ViewportWidgetInfo { return widget_info_; }
  [[nodiscard]] auto imageInfo() const -> ViewportImageInfo { return image_info_; }
  [[nodiscard]] auto interactionImageInfo() const -> ViewportImageInfo;
  // fitFraction = physical screen pixels per source-image pixel at fit (zoom
  // field 1.0). Uses the full image size (image_info_), not the 2K render
  // reference, because 100% is defined against the real image. Letterbox scale
  // is aspect-only so it is identical for the full image and the 2K reference
  // (crop tool off).
  [[nodiscard]] auto fitFraction() const -> float;
  // Zoom-field ceiling that maps to kMaxTrueZoom at the current image/viewport.
  [[nodiscard]] auto maxZoomField() const -> float;
  // Recompute maxZoomField() and push it to the view transform controller. Call
  // whenever image size or viewport metrics change.
  void applyMaxZoomToController();
  void applyViewTransformResult(const ViewTransformResult& result);
  void applyCropInteractionResult(const CropInteractionResult& result);
  void applyCursor(std::optional<Qt::CursorShape> cursor, bool unset);
  void stopZoomAnimation();
  // Stop an in-progress zoom animation and clear the routing-suppress flag, but
  // leave the interaction-settle timer running. Used by wheel/pinch input so a
  // no-op transform (e.g. zoom-in already at the ceiling) does NOT cancel a
  // pending DetailRefresh for the current viewport — only a real view change
  // (which restarts the timer via applyViewTransformResult) supersedes it.
  void interruptZoomAnimation();
  void scheduleViewChangeAfterInteractionSettles();
  void emitViewAndOverlay();
  // Phase 5D: report a user-driven view change for render routing. No-op when
  // interaction is disabled (bookkeeping setters call this directly only when a
  // real view/content change happened).
  void emitViewChange(ViewChangeKind kind);
  void updateViewportRenderRegionCache();
  void reconcileViewTransformForRenderReference();

  ViewerState viewer_state_{};
  ViewTransformController view_transform_controller_{};
  CropInteractionController crop_interaction_controller_{};
  ViewportWidgetInfo             widget_info_{1, 1, 1.0f};
  ViewportImageInfo              image_info_{0, 0};
  FramePresentationMode          presentation_mode_            = FramePresentationMode::FullFrame;
  // While a zoom animation is in progress, this also covers the synchronous
  // initial tick where zoom is still at fit. All zoomed view transforms use the
  // settled timer so DetailRefresh is scheduled once, not per interaction tick.
  bool                           suppress_view_change_routing_ = false;
  bool                           pointer_pan_active_            = false;
  bool                           pointer_pan_changed_           = false;
  bool                           view_input_sequence_active_    = false;
  bool                           view_input_sequence_changed_   = false;
  bool                           detail_roi_visible_           = false;
  QRectF                         detail_roi_uv_{0.0, 0.0, 1.0, 1.0};
  bool                           interaction_enabled_ = true;
  std::optional<Qt::CursorShape> cursor_{};
  QVariantAnimation* zoom_animation_ = nullptr;
  QTimer* click_toggle_timer_ = nullptr;
  QTimer* view_interaction_settle_timer_ = nullptr;
};

void RegisterEditorOverlayQmlTypes();

}  // namespace alcedo::editor_rhi
