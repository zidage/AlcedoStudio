//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <optional>

#include <QPoint>
#include <QPointF>
#include <QVector2D>
#include <Qt>

#include "ui/edit_viewer/viewer_state.hpp"
#include "ui/edit_viewer/viewport_mapper.hpp"

namespace alcedo {

struct ViewTransformAnimationState {
  float     start_zoom  = 1.0f;
  float     target_zoom = 1.0f;
  QVector2D start_pan   = {0.0f, 0.0f};
  QVector2D target_pan  = {0.0f, 0.0f};
};

struct ViewTransformResult {
  bool                       consumed                 = false;
  bool                       request_repaint          = false;
  bool                       start_click_toggle_timer = false;
  bool                       stop_click_toggle_timer  = false;
  std::optional<float>       emitted_zoom{};
  std::optional<Qt::CursorShape> cursor{};
  bool                       unset_cursor             = false;
  bool                       start_animation          = false;
  ViewTransformAnimationState animation{};
};

class ViewTransformController {
 public:
  static constexpr float kMinInteractiveZoom       = 1.0f;
  static constexpr float kMaxInteractiveZoom       = 8.0f;
  static constexpr float kWheelZoomStep            = 1.12f;
  static constexpr float kSingleClickZoomFactor    = 2.0f;
  static constexpr int   kClickDragThresholdPixels = 3;

  ViewTransformController() = default;

  // Override the interactive zoom ceiling. The default (kMaxInteractiveZoom)
  // keeps the legacy Qt Widgets editor and unit tests on their original 8x
  // fit-relative cap. The QML editor sets a dynamic ceiling derived from a
  // fixed *true-zoom* target (1:1 = 100%) so the zoom range is consistent across
  // image sizes and viewport DPIs regardless of the 2K preview downsample.
  void SetMaxZoom(float max_zoom) { max_zoom_ = max_zoom > kMinInteractiveZoom ? max_zoom : kMinInteractiveZoom; }

  auto HandleCtrlWheel(ViewerState& state, const ViewportWidgetInfo& widget_info,
                       const ViewportImageInfo& image_info, int wheel_delta,
                       const QPointF& anchor_widget_pos) -> ViewTransformResult;

  auto HandlePinchZoom(ViewerState& state, const ViewportWidgetInfo& widget_info,
                       const ViewportImageInfo& image_info, float zoom_delta,
                       const QPointF& anchor_widget_pos) -> ViewTransformResult;

  auto HandleWheelPan(ViewerState& state, const ViewportWidgetInfo& widget_info,
                      const ViewportImageInfo& image_info, const QPoint& pixel_delta)
      -> ViewTransformResult;

  auto HandlePanPress(bool interaction_blocked, const QPoint& mouse_pos) -> ViewTransformResult;

  auto HandlePanMove(ViewerState& state, const ViewportWidgetInfo& widget_info,
                     const ViewportImageInfo& image_info, const QPoint& mouse_pos)
      -> ViewTransformResult;

  auto HandlePanRelease(ViewerState& state, bool interaction_blocked, Qt::MouseButton button,
                        const QPointF& mouse_pos) -> ViewTransformResult;

  auto HandleDoubleClick(ViewerState& state, const ViewportWidgetInfo& widget_info,
                         const ViewportImageInfo& image_info, const QPointF& anchor_widget_pos)
      -> ViewTransformResult;

  auto HandleClickToggleTimeout(ViewerState& state, const ViewportWidgetInfo& widget_info,
                                const ViewportImageInfo& image_info) -> ViewTransformResult;

  auto ResetView(ViewerState& state) -> ViewTransformResult;
  auto HandleCropToolEnabledChanged(ViewerState& state, bool enabled) -> ViewTransformResult;

  auto ApplyAnimationProgress(ViewerState& state, const ViewportWidgetInfo& widget_info,
                              const ViewportImageInfo& image_info, float t)
      -> ViewTransformResult;

  auto ApplyAnimationFinished(ViewerState& state, const ViewportWidgetInfo& widget_info,
                              const ViewportImageInfo& image_info) -> ViewTransformResult;

 private:
  auto ApplyViewTransform(ViewerState& state, const ViewportWidgetInfo& widget_info,
                          const ViewportImageInfo& image_info, float zoom,
                          const QVector2D& pan, bool emit_zoom_signal) -> ViewTransformResult;

  auto AnimateViewTo(ViewerState& state, const ViewportWidgetInfo& widget_info,
                     const ViewportImageInfo& image_info, float target_zoom,
                     const std::optional<QPointF>& anchor_widget_pos,
                     const std::optional<QVector2D>& explicit_target_pan = std::nullopt)
      -> ViewTransformResult;

  void StopAnimation();

  bool      dragging_              = false;
  QPoint    last_mouse_pos_{};
  QPoint    drag_start_mouse_pos_{};
  bool      dragged_since_press_   = false;
  bool      click_zoom_toggle_active_ = false;
  float     click_zoom_restore_zoom_  = 1.0f;
  QVector2D click_zoom_restore_pan_   = {0.0f, 0.0f};
  float     double_click_zoom_target_ = kSingleClickZoomFactor;
  bool      double_click_zoom_in_next_ = true;
  QPointF   pending_click_toggle_pos_{};
  bool      pending_click_toggle_      = false;
  bool      suppress_next_click_release_toggle_ = false;
  bool      animation_active_          = false;
  ViewTransformAnimationState animation_state_{};
  float     max_zoom_                  = kMaxInteractiveZoom;
};

}  // namespace alcedo
