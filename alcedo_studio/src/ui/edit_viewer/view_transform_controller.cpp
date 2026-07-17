//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/edit_viewer/view_transform_controller.hpp"

#include <algorithm>
#include <cmath>

#include <QWheelEvent>

namespace alcedo {

auto ViewTransformController::HandleCtrlWheel(ViewerState& state,
                                              const ViewportWidgetInfo& widget_info,
                                              const ViewportImageInfo& image_info, int wheel_delta,
                                              const QPointF& anchor_widget_pos)
    -> ViewTransformResult {
  ViewTransformResult result;
  result.consumed = true;
  if (wheel_delta == 0) {
    return result;
  }

  const auto  view_state   = state.GetViewTransform();
  const float steps        =
      static_cast<float>(wheel_delta) / static_cast<float>(QWheelEvent::DefaultDeltasPerStep);
  const float target_zoom  =
      std::clamp(view_state.zoom * std::pow(kWheelZoomStep, steps), kMinInteractiveZoom,
                 kMaxInteractiveZoom);
  const QVector2D target_pan = ViewportMapper::ComputeAnchoredPan(
      anchor_widget_pos, widget_info, image_info, view_state.zoom, view_state.pan, target_zoom,
      view_state.pan);

  double_click_zoom_target_  = target_zoom;
  double_click_zoom_in_next_ = false;
  click_zoom_toggle_active_  = false;
  StopAnimation();
  return ApplyViewTransform(state, widget_info, image_info, target_zoom, target_pan, true);
}

auto ViewTransformController::HandlePinchZoom(ViewerState& state,
                                              const ViewportWidgetInfo& widget_info,
                                              const ViewportImageInfo& image_info, float zoom_delta,
                                              const QPointF& anchor_widget_pos)
    -> ViewTransformResult {
  ViewTransformResult result;
  result.consumed = true;
  if (std::abs(zoom_delta) < 1e-4f) {
    return result;
  }

  const auto view_state = state.GetViewTransform();
  // Qt docs: for ZoomNativeGesture, scale = scale * (1 + value)
  const float scale       = 1.0f + zoom_delta;
  const float target_zoom =
      std::clamp(view_state.zoom * scale, kMinInteractiveZoom, kMaxInteractiveZoom);
  const QVector2D target_pan = ViewportMapper::ComputeAnchoredPan(
      anchor_widget_pos, widget_info, image_info, view_state.zoom, view_state.pan, target_zoom,
      view_state.pan);

  double_click_zoom_target_  = target_zoom;
  double_click_zoom_in_next_ = false;
  click_zoom_toggle_active_  = false;
  StopAnimation();
  return ApplyViewTransform(state, widget_info, image_info, target_zoom, target_pan, true);
}

auto ViewTransformController::HandleWheelPan(ViewerState& state,
                                             const ViewportWidgetInfo& widget_info,
                                             const ViewportImageInfo& image_info,
                                             const QPoint& pixel_delta)
    -> ViewTransformResult {
  ViewTransformResult result;
  if (pixel_delta.isNull()) {
    return result;
  }

  // Qt 6 wheel pixelDelta is device-independent; keep the same logical NDC scale as pan.
  const float vw = std::max(1.0f, static_cast<float>(widget_info.widget_width));
  const float vh = std::max(1.0f, static_cast<float>(widget_info.widget_height));
  QVector2D   ndc_delta(2.0f * static_cast<float>(pixel_delta.x()) / vw,
                        -2.0f * static_cast<float>(pixel_delta.y()) / vh);

  const auto view_state = state.GetViewTransform();
  result.consumed = true;
  return ApplyViewTransform(state, widget_info, image_info, view_state.zoom,
                            view_state.pan + ndc_delta, false);
}

auto ViewTransformController::HandlePanPress(bool interaction_blocked, const QPoint& mouse_pos)
    -> ViewTransformResult {
  ViewTransformResult result;
  if (interaction_blocked) {
    result.consumed = true;
    return result;
  }

  pending_click_toggle_           = false;
  result.stop_click_toggle_timer  = true;
  StopAnimation();
  dragging_             = true;
  dragged_since_press_  = false;
  last_mouse_pos_       = mouse_pos;
  drag_start_mouse_pos_ = mouse_pos;
  result.cursor         = Qt::ClosedHandCursor;
  result.consumed       = true;
  return result;
}

auto ViewTransformController::HandlePanMove(ViewerState& state, const ViewportWidgetInfo& widget_info,
                                            const ViewportImageInfo& image_info,
                                            const QPoint& mouse_pos) -> ViewTransformResult {
  ViewTransformResult result;
  if (!dragging_) {
    return result;
  }

  const QPoint total_delta = mouse_pos - drag_start_mouse_pos_;
  if (!dragged_since_press_ &&
      total_delta.manhattanLength() >= kClickDragThresholdPixels) {
    dragged_since_press_      = true;
    click_zoom_toggle_active_ = false;
  }

  const QPoint delta = mouse_pos - last_mouse_pos_;
  last_mouse_pos_    = mouse_pos;

  result.consumed = true;
  if (!dragged_since_press_) {
    return result;
  }

  // Mouse/pointer deltas arrive in logical (device-independent) coordinates from both
  // QWidget and QML. Normalize by logical viewport size so the same logical drag yields
  // the same NDC pan at every devicePixelRatio. Absolute mapping multiplies both sides
  // by DPR; deltas must not mix logical numerators with physical denominators.
  const float vw = std::max(1.0f, static_cast<float>(widget_info.widget_width));
  const float vh = std::max(1.0f, static_cast<float>(widget_info.widget_height));
  QVector2D   ndc_delta(2.0f * static_cast<float>(delta.x()) / vw,
                        -2.0f * static_cast<float>(delta.y()) / vh);

  const auto view_state = state.GetViewTransform();
  return ApplyViewTransform(state, widget_info, image_info, view_state.zoom,
                            view_state.pan + ndc_delta, false);
}

auto ViewTransformController::HandlePanRelease(ViewerState& /*state*/, bool interaction_blocked,
                                               Qt::MouseButton button, const QPointF& mouse_pos)
    -> ViewTransformResult {
  ViewTransformResult result;
  if (!dragging_ || (button != Qt::LeftButton && button != Qt::MiddleButton)) {
    return result;
  }

  const bool left_click_without_drag = button == Qt::LeftButton && !dragged_since_press_;
  dragging_                          = false;
  dragged_since_press_               = false;
  result.unset_cursor                = true;
  result.consumed                    = true;

  if (left_click_without_drag) {
    if (suppress_next_click_release_toggle_) {
      suppress_next_click_release_toggle_ = false;
    } else if (!interaction_blocked) {
      pending_click_toggle_pos_ = mouse_pos;
      pending_click_toggle_     = true;
      result.start_click_toggle_timer = true;
    }
  }
  return result;
}

auto ViewTransformController::HandleDoubleClick(ViewerState& state,
                                                const ViewportWidgetInfo& widget_info,
                                                const ViewportImageInfo& image_info,
                                                const QPointF& anchor_widget_pos)
    -> ViewTransformResult {
  pending_click_toggle_               = false;
  suppress_next_click_release_toggle_ = true;
  ViewTransformResult result;
  result.stop_click_toggle_timer = true;

  float target_zoom = kSingleClickZoomFactor;
  bool  zoom_in     = true;
  target_zoom       = std::clamp(double_click_zoom_target_, kMinInteractiveZoom, kMaxInteractiveZoom);
  zoom_in           = double_click_zoom_in_next_;
  double_click_zoom_in_next_ = !double_click_zoom_in_next_;
  click_zoom_toggle_active_  = false;

  result = zoom_in ? AnimateViewTo(state, widget_info, image_info, target_zoom, anchor_widget_pos,
                                   std::nullopt)
                   : AnimateViewTo(state, widget_info, image_info, kMinInteractiveZoom, std::nullopt,
                                   QVector2D(0.0f, 0.0f));
  result.stop_click_toggle_timer = true;
  result.consumed                = true;
  return result;
}

auto ViewTransformController::HandleClickToggleTimeout(ViewerState& state,
                                                       const ViewportWidgetInfo& widget_info,
                                                       const ViewportImageInfo& image_info)
    -> ViewTransformResult {
  pending_click_toggle_ = false;
  if (click_zoom_toggle_active_) {
    click_zoom_toggle_active_ = false;
    return AnimateViewTo(state, widget_info, image_info, click_zoom_restore_zoom_, std::nullopt,
                         click_zoom_restore_pan_);
  }

  const auto view_state = state.GetViewTransform();
  click_zoom_restore_zoom_ = view_state.zoom;
  click_zoom_restore_pan_  = view_state.pan;
  click_zoom_toggle_active_ = true;
  return AnimateViewTo(state, widget_info, image_info, kSingleClickZoomFactor, pending_click_toggle_pos_,
                       std::nullopt);
}

auto ViewTransformController::ResetView(ViewerState& state) -> ViewTransformResult {
  click_zoom_toggle_active_           = false;
  pending_click_toggle_               = false;
  suppress_next_click_release_toggle_ = false;
  double_click_zoom_in_next_          = true;
  StopAnimation();
  ViewTransformResult result =
      ApplyViewTransform(state, {}, {}, kMinInteractiveZoom, QVector2D(0.0f, 0.0f), true);
  result.stop_click_toggle_timer = true;
  return result;
}

auto ViewTransformController::HandleCropToolEnabledChanged(ViewerState& state, bool enabled)
    -> ViewTransformResult {
  ViewTransformResult result;
  if (enabled) {
    StopAnimation();
    click_zoom_toggle_active_           = false;
    pending_click_toggle_               = false;
    suppress_next_click_release_toggle_ = false;
    double_click_zoom_in_next_          = true;
    const auto view_state               = state.GetViewTransform();
    result = ApplyViewTransform(state, {}, {}, kMinInteractiveZoom, QVector2D(0.0f, 0.0f), true);
    result.stop_click_toggle_timer = true;
    if (std::abs(view_state.zoom - kMinInteractiveZoom) <= 1e-5f &&
        view_state.pan.lengthSquared() <= 1e-8f) {
      result.emitted_zoom.reset();
    }
  } else {
    result.unset_cursor = true;
  }
  return result;
}

auto ViewTransformController::ApplyAnimationProgress(ViewerState& state,
                                                     const ViewportWidgetInfo& widget_info,
                                                     const ViewportImageInfo& image_info, float t)
    -> ViewTransformResult {
  const float progress = std::clamp(t, 0.0f, 1.0f);
  const float zoom = animation_state_.start_zoom +
                     ((animation_state_.target_zoom - animation_state_.start_zoom) * progress);
  const QVector2D pan =
      animation_state_.start_pan + ((animation_state_.target_pan - animation_state_.start_pan) * progress);
  return ApplyViewTransform(state, widget_info, image_info, zoom, pan, true);
}

auto ViewTransformController::ApplyAnimationFinished(ViewerState& state,
                                                     const ViewportWidgetInfo& widget_info,
                                                     const ViewportImageInfo& image_info)
    -> ViewTransformResult {
  animation_active_ = false;
  return ApplyViewTransform(state, widget_info, image_info, animation_state_.target_zoom,
                            animation_state_.target_pan, true);
}

auto ViewTransformController::ApplyViewTransform(ViewerState& state,
                                                 const ViewportWidgetInfo& widget_info,
                                                 const ViewportImageInfo& image_info, float zoom,
                                                 const QVector2D& pan, bool emit_zoom_signal)
    -> ViewTransformResult {
  const auto clamped_pan = ViewportMapper::ClampPanForZoom(widget_info, image_info, zoom, pan,
                                                           kMinInteractiveZoom, kMaxInteractiveZoom);
  const float clamped_zoom = std::clamp(zoom, kMinInteractiveZoom, kMaxInteractiveZoom);
  const auto  previous     = state.GetViewTransform();
  state.SetViewTransform(clamped_zoom, clamped_pan);

  ViewTransformResult result;
  result.consumed        = true;
  result.request_repaint = true;
  if (emit_zoom_signal && std::abs(previous.zoom - clamped_zoom) > 1e-5f) {
    result.emitted_zoom = clamped_zoom;
  }
  return result;
}

auto ViewTransformController::AnimateViewTo(ViewerState& state, const ViewportWidgetInfo& widget_info,
                                            const ViewportImageInfo& image_info, float target_zoom,
                                            const std::optional<QPointF>& anchor_widget_pos,
                                            const std::optional<QVector2D>& explicit_target_pan)
    -> ViewTransformResult {
  const auto  view_state  = state.GetViewTransform();
  const float clamped_zoom =
      std::clamp(target_zoom, kMinInteractiveZoom, kMaxInteractiveZoom);
  QVector2D target_pan = explicit_target_pan.value_or(view_state.pan);
  if (!explicit_target_pan.has_value() && anchor_widget_pos.has_value()) {
    target_pan = ViewportMapper::ComputeAnchoredPan(*anchor_widget_pos, widget_info, image_info,
                                                    view_state.zoom, view_state.pan, clamped_zoom,
                                                    view_state.pan);
  }
  target_pan = ViewportMapper::ClampPanForZoom(widget_info, image_info, clamped_zoom, target_pan,
                                               kMinInteractiveZoom, kMaxInteractiveZoom);

  if (std::abs(view_state.zoom - clamped_zoom) <= 1e-5f &&
      (view_state.pan - target_pan).lengthSquared() <= 1e-8f) {
    return ApplyViewTransform(state, widget_info, image_info, clamped_zoom, target_pan, true);
  }

  animation_active_             = true;
  animation_state_.start_zoom   = view_state.zoom;
  animation_state_.target_zoom  = clamped_zoom;
  animation_state_.start_pan    = view_state.pan;
  animation_state_.target_pan   = target_pan;

  ViewTransformResult result;
  result.consumed        = true;
  result.start_animation = true;
  result.animation       = animation_state_;
  return result;
}

void ViewTransformController::StopAnimation() { animation_active_ = false; }

}  // namespace alcedo
