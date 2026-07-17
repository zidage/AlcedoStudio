//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/editor_rhi/editor_interaction_controller.hpp"

#include "ui/editor_rhi/editor_overlay_item.hpp"

#include <QApplication>
#include <QtQml/qqml.h>

#include <algorithm>
#include <cmath>
#include <mutex>

namespace alcedo::editor_rhi {
namespace {

constexpr float kMinInteractiveZoom = ViewTransformController::kMinInteractiveZoom;
constexpr float kMaxInteractiveZoom = ViewTransformController::kMaxInteractiveZoom;

}  // namespace

EditorInteractionController::EditorInteractionController(QObject* parent) : QObject(parent) {
  zoom_animation_ = new QVariantAnimation(this);
  zoom_animation_->setEasingCurve(QEasingCurve::OutCubic);
  connect(zoom_animation_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
    const auto result = view_transform_controller_.ApplyAnimationProgress(
        viewer_state_, widgetInfo(), interactionImageInfo(),
        std::clamp(static_cast<float>(value.toDouble()), 0.0f, 1.0f));
    applyViewTransformResult(result);
  });
  connect(zoom_animation_, &QVariantAnimation::finished, this, [this]() {
    const auto result = view_transform_controller_.ApplyAnimationFinished(
        viewer_state_, widgetInfo(), interactionImageInfo());
    applyViewTransformResult(result);
  });

  click_toggle_timer_ = new QTimer(this);
  click_toggle_timer_->setSingleShot(true);
  connect(click_toggle_timer_, &QTimer::timeout, this, [this]() {
    const auto result = view_transform_controller_.HandleClickToggleTimeout(
        viewer_state_, widgetInfo(), interactionImageInfo());
    applyViewTransformResult(result);
  });
}

auto EditorInteractionController::zoom() const -> float {
  return viewer_state_.GetViewTransform().zoom;
}

auto EditorInteractionController::panX() const -> float {
  return viewer_state_.GetViewTransform().pan.x();
}

auto EditorInteractionController::panY() const -> float {
  return viewer_state_.GetViewTransform().pan.y();
}

auto EditorInteractionController::cropToolEnabled() const -> bool {
  return viewer_state_.GetCropOverlay().tool_enabled;
}

auto EditorInteractionController::cropOverlayVisible() const -> bool {
  return viewer_state_.GetCropOverlay().overlay_visible;
}

auto EditorInteractionController::cropRectNormalized() const -> QRectF {
  return viewer_state_.GetCropOverlay().rect;
}

auto EditorInteractionController::cropRotationDegrees() const -> float {
  return viewer_state_.GetCropOverlay().rotation_degrees;
}

auto EditorInteractionController::aspectLocked() const -> bool {
  return viewer_state_.GetCropOverlay().aspect_locked;
}

auto EditorInteractionController::aspectRatio() const -> float {
  return viewer_state_.GetCropOverlay().aspect_ratio;
}

auto EditorInteractionController::metricAspect() const -> float {
  return viewer_state_.GetCropOverlay().metric_aspect;
}

auto EditorInteractionController::rotateHandleItemPos() const -> QPointF {
  const auto geometry = overlayGeometry();
  if (!geometry.crop_corners_valid) {
    return {};
  }
  return geometry.rotate_handle_widget;
}

auto EditorInteractionController::overlayGeometryValid() const -> bool {
  return overlayGeometry().image_rect_valid;
}

void EditorInteractionController::setCropToolEnabled(bool enabled) {
  if (viewer_state_.GetCropOverlay().tool_enabled == enabled) {
    return;
  }
  stopZoomAnimation();
  viewer_state_.SetCropToolEnabled(enabled);
  const auto result = view_transform_controller_.HandleCropToolEnabledChanged(viewer_state_, enabled);
  applyViewTransformResult(result);
  emit cropToolChanged();
  emit cropChanged();
  emit overlayGeometryChanged();
  emit viewStateChanged();
}

void EditorInteractionController::setCropOverlayVisible(bool visible) {
  if (viewer_state_.GetCropOverlay().overlay_visible == visible) {
    return;
  }
  viewer_state_.SetCropOverlayVisible(visible);
  emit cropChanged();
  emit overlayGeometryChanged();
  emit viewStateChanged();
}

void EditorInteractionController::setCropRectNormalized(const QRectF& rect) {
  const QRectF clamped = CropGeometry::ClampCropRect(rect);
  auto crop = viewer_state_.GetCropOverlay();
  if (crop.rect == clamped) {
    return;
  }
  crop.rect = clamped;
  viewer_state_.SetCropOverlayState(crop);
  emit cropChanged();
  emit cropRectCommitted(clamped, true);
  emit overlayGeometryChanged();
  emit viewStateChanged();
}

void EditorInteractionController::setCropRotationDegrees(float degrees) {
  const float normalized = CropGeometry::NormalizeAngleDegrees(degrees);
  auto crop = viewer_state_.GetCropOverlay();
  if (std::abs(crop.rotation_degrees - normalized) < 1.0e-5f) {
    return;
  }
  crop.rotation_degrees = normalized;
  crop.rect = CropGeometry::ClampCropRectForRotation(crop.rect, crop.rotation_degrees,
                                                     crop.metric_aspect);
  viewer_state_.SetCropOverlayState(crop);
  emit cropChanged();
  emit cropRotationCommitted(normalized, true);
  emit overlayGeometryChanged();
  emit viewStateChanged();
}

void EditorInteractionController::setCropAspectLock(bool enabled, float aspect_ratio) {
  auto crop = viewer_state_.GetCropOverlay();
  const float ratio = CropGeometry::ClampAspectRatio(aspect_ratio);
  if (crop.aspect_locked == enabled && std::abs(crop.aspect_ratio - ratio) < 1.0e-5f) {
    return;
  }
  crop.aspect_locked = enabled;
  crop.aspect_ratio = ratio;
  viewer_state_.SetCropOverlayState(crop);
  emit cropChanged();
  emit overlayGeometryChanged();
  emit viewStateChanged();
}

void EditorInteractionController::setPresentationMode(int mode) {
  const auto next = mode == static_cast<int>(FramePresentationMode::RoiFrame)
                        ? FramePresentationMode::RoiFrame
                        : FramePresentationMode::FullFrame;
  if (presentation_mode_ == next) {
    return;
  }
  presentation_mode_ = next;
  emit overlayGeometryChanged();
  emit viewStateChanged();
}

void EditorInteractionController::setDetailRoiVisible(bool visible) {
  if (detail_roi_visible_ == visible) {
    return;
  }
  detail_roi_visible_ = visible;
  emit overlayGeometryChanged();
}

void EditorInteractionController::setDetailRoiNormalized(const QRectF& rect_uv) {
  const QRectF next = rect_uv.normalized();
  if (detail_roi_uv_ == next) {
    return;
  }
  detail_roi_uv_ = next;
  emit overlayGeometryChanged();
}

void EditorInteractionController::setInteractionEnabled(bool enabled) {
  if (interaction_enabled_ == enabled) {
    return;
  }
  interaction_enabled_ = enabled;
  if (!enabled) {
    crop_interaction_controller_.Cancel();
    stopZoomAnimation();
    applyCursor(std::nullopt, true);
  }
  emit interactionEnabledChanged();
}

void EditorInteractionController::setViewportMetrics(qreal width, qreal height,
                                                     qreal devicePixelRatio) {
  const int w = std::max(1, static_cast<int>(std::lround(width)));
  const int h = std::max(1, static_cast<int>(std::lround(height)));
  const float dpr = static_cast<float>(std::max(devicePixelRatio, 1.0e-4));
  if (widget_info_.widget_width == w && widget_info_.widget_height == h &&
      std::abs(widget_info_.device_pixel_ratio - dpr) < 1.0e-5f) {
    return;
  }
  widget_info_ = {w, h, dpr};
  reconcileViewTransformForRenderReference();
  updateViewportRenderRegionCache();
  emit viewportMetricsChanged();
  emit overlayGeometryChanged();
  emit viewStateChanged();
}

void EditorInteractionController::setImageSize(int width, int height) {
  width = std::max(0, width);
  height = std::max(0, height);
  if (image_info_.image_width == width && image_info_.image_height == height) {
    return;
  }
  image_info_ = {width, height};
  if (width > 0 && height > 0) {
    auto crop = viewer_state_.GetCropOverlay();
    crop.metric_aspect = CropGeometry::SafeAspect(width, height);
    viewer_state_.SetCropOverlayState(crop);
  }
  reconcileViewTransformForRenderReference();
  updateViewportRenderRegionCache();
  emit imageGeometryChanged();
  emit overlayGeometryChanged();
  emit viewStateChanged();
}

void EditorInteractionController::setRenderReferenceSize(int width, int height) {
  width = std::max(0, width);
  height = std::max(0, height);
  const auto snapshot = viewer_state_.Snapshot();
  if (snapshot.render_reference_width == width && snapshot.render_reference_height == height) {
    return;
  }
  viewer_state_.SetRenderReferenceSize(width, height);
  reconcileViewTransformForRenderReference();
  updateViewportRenderRegionCache();
  emit imageGeometryChanged();
  emit overlayGeometryChanged();
  emit viewStateChanged();
}

void EditorInteractionController::handleHoverMove(qreal x, qreal y) {
  if (!interaction_enabled_) {
    return;
  }
  const QPointF pos(x, y);
  const auto snapshot = overlaySnapshot();
  const auto geometry = EditViewerOverlayGeometry::Build(snapshot);
  const auto hover = EditViewerOverlayGeometry::ComputeHover(snapshot, geometry, pos);
  applyCursor(hover.cursor, !hover.cursor.has_value());
}

void EditorInteractionController::handlePress(qreal x, qreal y, int button) {
  if (!interaction_enabled_) {
    return;
  }
  const QPointF pos(x, y);
  const auto qt_button = static_cast<Qt::MouseButton>(button);

  if (qt_button == Qt::LeftButton) {
    const auto snapshot = overlaySnapshot();
    const auto geometry = EditViewerOverlayGeometry::Build(snapshot);
    const auto hover = EditViewerOverlayGeometry::ComputeHover(snapshot, geometry, pos);
    const CropPressContext press_context{pos, hover.image_uv, hover.crop_hit, hover.inside_image};
    const auto crop_result =
        crop_interaction_controller_.HandlePress(viewer_state_, imageInfo(), press_context);
    if (crop_result.consumed) {
      applyCropInteractionResult(crop_result);
      return;
    }
  }

  if (qt_button == Qt::LeftButton || qt_button == Qt::MiddleButton) {
    const auto crop_state = viewer_state_.GetCropOverlay();
    stopZoomAnimation();
    const auto result = view_transform_controller_.HandlePanPress(
        crop_state.tool_enabled && crop_state.overlay_visible, pos.toPoint());
    if (result.consumed) {
      applyViewTransformResult(result);
    }
  }
}

void EditorInteractionController::handleMove(qreal x, qreal y, int buttons) {
  if (!interaction_enabled_) {
    return;
  }
  const QPointF pos(x, y);
  const auto qt_buttons = static_cast<Qt::MouseButtons>(buttons);

  const auto crop_result = crop_interaction_controller_.HandleMove(
      viewer_state_, widgetInfo(), imageInfo(), qt_buttons, pos);
  if (crop_result.consumed) {
    applyCropInteractionResult(crop_result);
    return;
  }

  const auto result = view_transform_controller_.HandlePanMove(
      viewer_state_, widgetInfo(), interactionImageInfo(), pos.toPoint());
  if (result.consumed) {
    applyViewTransformResult(result);
    return;
  }

  handleHoverMove(x, y);
}

void EditorInteractionController::handleRelease(qreal x, qreal y, int button) {
  if (!interaction_enabled_) {
    return;
  }
  const QPointF pos(x, y);
  const auto qt_button = static_cast<Qt::MouseButton>(button);

  if (qt_button == Qt::LeftButton) {
    const auto crop_result = crop_interaction_controller_.HandleRelease(viewer_state_);
    if (crop_result.consumed) {
      applyCropInteractionResult(crop_result);
    }
  }

  const auto crop_state = viewer_state_.GetCropOverlay();
  const auto result = view_transform_controller_.HandlePanRelease(
      viewer_state_, crop_state.tool_enabled && crop_state.overlay_visible, qt_button, pos);
  if (result.consumed) {
    applyViewTransformResult(result);
  }

  handleHoverMove(x, y);
}

void EditorInteractionController::handleDoubleTap(qreal x, qreal y) {
  if (!interaction_enabled_) {
    return;
  }
  const auto crop_state = viewer_state_.GetCropOverlay();
  if (crop_state.tool_enabled && crop_state.overlay_visible) {
    const auto crop_result = crop_interaction_controller_.HandleDoubleClick(viewer_state_);
    if (crop_result.consumed) {
      applyCropInteractionResult(crop_result);
      return;
    }
  }

  stopZoomAnimation();
  const auto result = view_transform_controller_.HandleDoubleClick(
      viewer_state_, widgetInfo(), interactionImageInfo(), QPointF(x, y));
  applyViewTransformResult(result);
}

void EditorInteractionController::handleWheel(qreal x, qreal y, int angleDeltaY, int pixelDeltaX,
                                              int pixelDeltaY, int modifiers, bool synthesized) {
  if (!interaction_enabled_) {
    return;
  }
  const QPointF pos(x, y);
  const auto qt_modifiers = static_cast<Qt::KeyboardModifiers>(modifiers);

  if ((qt_modifiers & Qt::ControlModifier) == Qt::ControlModifier) {
    stopZoomAnimation();
    const auto result = view_transform_controller_.HandleCtrlWheel(
        viewer_state_, widgetInfo(), interactionImageInfo(), angleDeltaY, pos);
    applyViewTransformResult(result);
    return;
  }

  if (synthesized) {
    QPoint pixel_delta(pixelDeltaX, pixelDeltaY);
    if (pixel_delta.isNull() && angleDeltaY != 0) {
      pixel_delta = QPoint(0, angleDeltaY / 4);
    }
    if (!pixel_delta.isNull()) {
      stopZoomAnimation();
      const auto result = view_transform_controller_.HandleWheelPan(
          viewer_state_, widgetInfo(), interactionImageInfo(), pixel_delta);
      applyViewTransformResult(result);
    }
  }
}

void EditorInteractionController::handlePinch(qreal x, qreal y, qreal scaleDelta) {
  if (!interaction_enabled_) {
    return;
  }
  stopZoomAnimation();
  const float value = static_cast<float>(scaleDelta);
  if (std::abs(value) <= 1.0e-4f) {
    return;
  }
  const auto result = view_transform_controller_.HandlePinchZoom(
      viewer_state_, widgetInfo(), interactionImageInfo(), value, QPointF(x, y));
  applyViewTransformResult(result);
}

void EditorInteractionController::handleLeave() { applyCursor(std::nullopt, true); }

void EditorInteractionController::resetView() {
  stopZoomAnimation();
  const auto result = view_transform_controller_.ResetView(viewer_state_);
  applyViewTransformResult(result);
}

void EditorInteractionController::resetCropToFull() {
  setCropRectNormalized(QRectF(0.0, 0.0, 1.0, 1.0));
  setCropRotationDegrees(0.0f);
  setCropAspectLock(false, 1.0f);
}

QPointF EditorInteractionController::itemPointToImageUv(qreal x, qreal y) const {
  const auto view = viewer_state_.GetViewTransform();
  float zoom = view.zoom;
  QVector2D pan = view.pan;
  if (presentation_mode_ == FramePresentationMode::RoiFrame) {
    zoom = 1.0f;
    pan = QVector2D(0.0f, 0.0f);
  }
  const auto uv = ViewportMapper::WidgetPointToImageUv(QPointF(x, y), widgetInfo(), imageInfo(),
                                                       zoom, pan);
  return uv.value_or(QPointF());
}

QPointF EditorInteractionController::imageUvToItemPoint(qreal u, qreal v) const {
  const auto view = viewer_state_.GetViewTransform();
  float zoom = view.zoom;
  QVector2D pan = view.pan;
  if (presentation_mode_ == FramePresentationMode::RoiFrame) {
    zoom = 1.0f;
    pan = QVector2D(0.0f, 0.0f);
  }
  const auto point =
      ViewportMapper::ImageUvToWidgetPoint(QPointF(u, v), widgetInfo(), imageInfo(), zoom, pan);
  return point.value_or(QPointF());
}

bool EditorInteractionController::isItemPointInsideImage(qreal x, qreal y) const {
  const auto view = viewer_state_.GetViewTransform();
  float zoom = view.zoom;
  QVector2D pan = view.pan;
  if (presentation_mode_ == FramePresentationMode::RoiFrame) {
    zoom = 1.0f;
    pan = QVector2D(0.0f, 0.0f);
  }
  return ViewportMapper::WidgetPointToImageUv(QPointF(x, y), widgetInfo(), imageInfo(), zoom, pan)
      .has_value();
}

auto EditorInteractionController::overlaySnapshot() const -> EditViewerOverlaySnapshot {
  EditViewerOverlaySnapshot snapshot;
  snapshot.viewer_state = viewer_state_.Snapshot();
  snapshot.widget_info = widget_info_;
  snapshot.image_info = image_info_;
  snapshot.presentation_mode = presentation_mode_;
  snapshot.detail_roi_visible = detail_roi_visible_;
  snapshot.detail_roi_uv = detail_roi_uv_;
  return snapshot;
}

auto EditorInteractionController::overlayGeometry() const -> CropOverlayWidgetGeometry {
  return EditViewerOverlayGeometry::Build(overlaySnapshot());
}

auto EditorInteractionController::viewerViewState() const -> ViewerViewState {
  ViewerViewState state;
  state.snapshot = viewer_state_.Snapshot();
  state.prefer_interactive_primary = state.snapshot.view_transform.zoom > 1.0f + 1.0e-4f;
  state.allow_detail_patch = state.prefer_interactive_primary;
  return state;
}

void EditorInteractionController::applyViewTransformForTest(float next_zoom, float pan_x,
                                                            float pan_y) {
  stopZoomAnimation();
  const float clamped_zoom = std::clamp(next_zoom, kMinInteractiveZoom, kMaxInteractiveZoom);
  const QVector2D pan = ViewportMapper::ClampPanForZoom(
      widgetInfo(), interactionImageInfo(), clamped_zoom, QVector2D(pan_x, pan_y),
      kMinInteractiveZoom, kMaxInteractiveZoom);
  viewer_state_.SetViewTransform(clamped_zoom, pan);
  updateViewportRenderRegionCache();
  emitViewAndOverlay();
  emit viewZoomChanged(clamped_zoom);
}

auto EditorInteractionController::interactionImageInfo() const -> ViewportImageInfo {
  const auto snapshot = viewer_state_.Snapshot();
  if (snapshot.render_reference_width > 0 && snapshot.render_reference_height > 0) {
    return {snapshot.render_reference_width, snapshot.render_reference_height};
  }
  return image_info_;
}

void EditorInteractionController::applyViewTransformResult(const ViewTransformResult& result) {
  if (result.stop_click_toggle_timer && click_toggle_timer_ && click_toggle_timer_->isActive()) {
    click_toggle_timer_->stop();
  }
  if (result.start_click_toggle_timer && click_toggle_timer_) {
    const int interval =
        QApplication::instance() ? QApplication::doubleClickInterval() : 400;
    click_toggle_timer_->start(interval);
  }
  applyCursor(result.cursor, result.unset_cursor);
  if (result.start_animation && zoom_animation_) {
    zoom_animation_->setDuration(kZoomAnimationDurationMs);
    zoom_animation_->setStartValue(0.0);
    zoom_animation_->setEndValue(1.0);
    zoom_animation_->start();
  }
  if (result.request_repaint) {
    updateViewportRenderRegionCache();
    emitViewAndOverlay();
  }
  if (result.emitted_zoom.has_value()) {
    emit viewZoomChanged(*result.emitted_zoom);
  }
}

void EditorInteractionController::applyCropInteractionResult(const CropInteractionResult& result) {
  applyCursor(result.cursor, result.unset_cursor);
  if (result.rect_changed.has_value()) {
    emit cropRectCommitted(*result.rect_changed, result.rect_is_final);
    emit cropChanged();
  }
  if (result.rotation_changed.has_value()) {
    emit cropRotationCommitted(*result.rotation_changed, result.rotation_is_final);
    emit cropChanged();
  }
  if (result.request_repaint) {
    emit overlayGeometryChanged();
    emit viewStateChanged();
  }
}

void EditorInteractionController::applyCursor(std::optional<Qt::CursorShape> cursor, bool unset) {
  if (unset) {
    if (!cursor_.has_value()) {
      return;
    }
    cursor_.reset();
    emit cursorChanged();
    return;
  }
  if (cursor.has_value()) {
    if (cursor_ == cursor) {
      return;
    }
    cursor_ = cursor;
    emit cursorChanged();
  }
}

void EditorInteractionController::stopZoomAnimation() {
  if (zoom_animation_ && zoom_animation_->state() == QAbstractAnimation::Running) {
    zoom_animation_->stop();
  }
}

void EditorInteractionController::emitViewAndOverlay() {
  emit viewChanged();
  emit overlayGeometryChanged();
  emit viewStateChanged();
}

void EditorInteractionController::updateViewportRenderRegionCache() {
  const auto snapshot = viewer_state_.Snapshot();
  if (snapshot.render_reference_width <= 0 || snapshot.render_reference_height <= 0) {
    viewer_state_.SetViewportRenderRegion(std::nullopt);
    return;
  }
  viewer_state_.SetViewportRenderRegion(ViewportMapper::ComputeViewportRenderRegion(
      widgetInfo(), snapshot.view_transform.zoom, snapshot.view_transform.pan,
      snapshot.render_reference_width, snapshot.render_reference_height));
}

void EditorInteractionController::reconcileViewTransformForRenderReference() {
  const auto snapshot = viewer_state_.Snapshot();
  if (snapshot.render_reference_width <= 0 || snapshot.render_reference_height <= 0) {
    return;
  }
  const float clamped_zoom =
      std::clamp(snapshot.view_transform.zoom, kMinInteractiveZoom, kMaxInteractiveZoom);
  QVector2D clamped_pan = ViewportMapper::ClampPanForZoom(
      widgetInfo(), {snapshot.render_reference_width, snapshot.render_reference_height},
      clamped_zoom, snapshot.view_transform.pan, kMinInteractiveZoom, kMaxInteractiveZoom);
  if (clamped_zoom <= (kMinInteractiveZoom + 1.0e-4f)) {
    clamped_pan = QVector2D(0.0f, 0.0f);
  }
  const bool zoom_changed = std::abs(clamped_zoom - snapshot.view_transform.zoom) > 1.0e-5f;
  const bool pan_changed = (clamped_pan - snapshot.view_transform.pan).lengthSquared() > 1.0e-8f;
  if (!zoom_changed && !pan_changed) {
    return;
  }
  stopZoomAnimation();
  viewer_state_.SetViewTransform(clamped_zoom, clamped_pan);
}

void RegisterEditorOverlayQmlTypes() {
  static std::once_flag once;
  std::call_once(once, [] {
    qmlRegisterModule("Alcedo.Main", 1, 0);
    qmlRegisterType<EditorInteractionController>("Alcedo.Main", 1, 0,
                                                 "EditorInteractionController");
    qmlRegisterType<EditorOverlayItem>("Alcedo.Main", 1, 0, "EditorOverlayItem");
  });
}

}  // namespace alcedo::editor_rhi
