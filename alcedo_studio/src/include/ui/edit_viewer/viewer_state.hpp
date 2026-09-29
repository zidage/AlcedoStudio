//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <mutex>
#include <optional>

#include <QRectF>
#include <QVector2D>

#include "ui/edit_viewer/frame_sink.hpp"

namespace alcedo {

struct ViewTransformState {
  float     zoom = 1.0f;
  QVector2D pan  = {0.0f, 0.0f};
};

/**
 * @brief Geometry panel crop frame shown on the viewport.
 *
 * @p rect and @p rotation_degrees use the document representation (ImageGeometryModel).
 * @p aspect_ratio is width / height of the crop frame in source pixels.
 */
struct CropOverlayState {
  bool   tool_enabled       = false;
  bool   overlay_visible    = false;
  QRectF rect               = QRectF(0.0, 0.0, 1.0, 1.0);
  float  rotation_degrees   = 0.0f;
  bool   aspect_locked      = false;
  float  aspect_ratio       = 1.0f;
};

struct ViewerStateSnapshot {
  ViewTransformState                  view_transform{};
  CropOverlayState                    crop_overlay{};
  std::optional<ViewportRenderRegion> viewport_render_region_cache{};
  int                                 render_reference_width  = 0;
  int                                 render_reference_height = 0;
};

class ViewerState {
 public:
  ViewerState() = default;

  auto Snapshot() const -> ViewerStateSnapshot {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
  }

  auto GetViewZoom() const -> float {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_.view_transform.zoom;
  }

  auto GetViewTransform() const -> ViewTransformState {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_.view_transform;
  }

  void SetViewTransform(float zoom, const QVector2D& pan) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.view_transform.zoom = zoom;
    state_.view_transform.pan  = pan;
  }

  auto GetCropOverlay() const -> CropOverlayState {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_.crop_overlay;
  }

  void SetCropToolEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.crop_overlay.tool_enabled = enabled;
  }

  void SetCropOverlayVisible(bool visible) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.crop_overlay.overlay_visible = visible;
  }

  void SetCropOverlayRect(const QRectF& rect) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.crop_overlay.rect = rect;
  }

  void SetCropOverlayRotationDegrees(float angle_degrees) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.crop_overlay.rotation_degrees = angle_degrees;
  }

  void SetCropOverlayAspectLock(bool enabled, float aspect_ratio) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.crop_overlay.aspect_locked = enabled;
    state_.crop_overlay.aspect_ratio  = aspect_ratio;
  }

  void SetCropOverlayState(const CropOverlayState& crop_overlay) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.crop_overlay = crop_overlay;
  }

  auto GetViewportRenderRegion() const -> std::optional<ViewportRenderRegion> {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_.viewport_render_region_cache;
  }

  void SetViewportRenderRegion(std::optional<ViewportRenderRegion> region) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.viewport_render_region_cache = region;
  }

  void SetRenderReferenceSize(int width, int height) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.render_reference_width  = width;
    state_.render_reference_height = height;
  }

 private:
  mutable std::mutex mutex_;
  ViewerStateSnapshot state_{};
};

}  // namespace alcedo
