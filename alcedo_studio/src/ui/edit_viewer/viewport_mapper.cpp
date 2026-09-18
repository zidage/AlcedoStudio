//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/edit_viewer/viewport_mapper.hpp"

#include <algorithm>
#include <cmath>

namespace alcedo {

auto ViewportMapper::Clamp01(float value) -> float { return std::clamp(value, 0.0f, 1.0f); }

auto ViewportMapper::ComputeLetterboxScale(const ViewportWidgetInfo& widget_info,
                                           const ViewportImageInfo& image_info) -> LetterboxScale {
  const float vw = std::max(1.0f, static_cast<float>(widget_info.widget_width) *
                                      std::max(widget_info.device_pixel_ratio, 1e-4f));
  const float vh = std::max(1.0f, static_cast<float>(widget_info.widget_height) *
                                      std::max(widget_info.device_pixel_ratio, 1e-4f));
  const float img_w = static_cast<float>(std::max(1, image_info.image_width));
  const float img_h = static_cast<float>(std::max(1, image_info.image_height));
  const float win_aspect = vw / vh;
  const float img_aspect = img_w / img_h;

  LetterboxScale scale{};
  if (img_aspect > win_aspect) {
    scale.y = win_aspect / img_aspect;
  } else {
    scale.x = img_aspect / win_aspect;
  }
  scale.x = std::max(scale.x, 1e-4f);
  scale.y = std::max(scale.y, 1e-4f);
  return scale;
}

auto ViewportMapper::WidgetPointToImageUv(const QPointF& widget_pos,
                                          const ViewportWidgetInfo& widget_info,
                                          const ViewportImageInfo& image_info, float zoom,
                                          const QVector2D& pan) -> std::optional<QPointF> {
  if (image_info.image_width <= 0 || image_info.image_height <= 0 || widget_info.widget_width <= 0 ||
      widget_info.widget_height <= 0) {
    return std::nullopt;
  }

  const float dpr = std::max(widget_info.device_pixel_ratio, 1e-4f);
  const float vw = std::max(1.0f, static_cast<float>(widget_info.widget_width) * dpr);
  const float vh = std::max(1.0f, static_cast<float>(widget_info.widget_height) * dpr);
  const auto  scale = ComputeLetterboxScale(widget_info, image_info);

  const float px   = static_cast<float>(widget_pos.x()) * dpr;
  const float py   = static_cast<float>(widget_pos.y()) * dpr;
  const float ndc_x = (2.0f * px / vw) - 1.0f;
  const float ndc_y = 1.0f - (2.0f * py / vh);

  const float img_x = (ndc_x - pan.x()) / (scale.x * std::max(zoom, 1e-4f));
  const float img_y = (ndc_y - pan.y()) / (scale.y * std::max(zoom, 1e-4f));

  return QPointF((img_x + 1.0f) * 0.5f, (1.0f - img_y) * 0.5f);
}

auto ViewportMapper::ImageUvToWidgetPoint(const QPointF& uv, const ViewportWidgetInfo& widget_info,
                                          const ViewportImageInfo& image_info, float zoom,
                                          const QVector2D& pan, bool clamp_uv)
    -> std::optional<QPointF> {
  if (image_info.image_width <= 0 || image_info.image_height <= 0 || widget_info.widget_width <= 0 ||
      widget_info.widget_height <= 0) {
    return std::nullopt;
  }

  const float dpr = std::max(widget_info.device_pixel_ratio, 1e-4f);
  const float vw = std::max(1.0f, static_cast<float>(widget_info.widget_width) * dpr);
  const float vh = std::max(1.0f, static_cast<float>(widget_info.widget_height) * dpr);
  const auto  scale = ComputeLetterboxScale(widget_info, image_info);

  const float u =
      clamp_uv ? Clamp01(static_cast<float>(uv.x())) : static_cast<float>(uv.x());
  const float v =
      clamp_uv ? Clamp01(static_cast<float>(uv.y())) : static_cast<float>(uv.y());
  const float img_x = (2.0f * u) - 1.0f;
  const float img_y = 1.0f - (2.0f * v);
  const float ndc_x = (img_x * scale.x * std::max(zoom, 1e-4f)) + pan.x();
  const float ndc_y = (img_y * scale.y * std::max(zoom, 1e-4f)) + pan.y();

  return QPointF((((ndc_x + 1.0f) * 0.5f) * vw) / dpr, (((1.0f - ndc_y) * 0.5f) * vh) / dpr);
}

auto ViewportMapper::ClampPanForZoom(const ViewportWidgetInfo& widget_info,
                                     const ViewportImageInfo& image_info, float zoom,
                                     const QVector2D& pan, float min_zoom, float max_zoom)
    -> QVector2D {
  if (image_info.image_width <= 0 || image_info.image_height <= 0) {
    return QVector2D(0.0f, 0.0f);
  }

  const auto  scale        = ComputeLetterboxScale(widget_info, image_info);
  const float clamped_zoom = std::clamp(zoom, min_zoom, max_zoom);
  const float max_pan_x    = std::max(0.0f, (scale.x * clamped_zoom) - 1.0f);
  const float max_pan_y    = std::max(0.0f, (scale.y * clamped_zoom) - 1.0f);
  return QVector2D(std::clamp(pan.x(), -max_pan_x, max_pan_x),
                   std::clamp(pan.y(), -max_pan_y, max_pan_y));
}

auto ViewportMapper::ComputeAnchoredPan(const QPointF& anchor_widget_pos,
                                        const ViewportWidgetInfo& widget_info,
                                        const ViewportImageInfo& image_info, float current_zoom,
                                        const QVector2D& current_pan, float target_zoom,
                                        const QVector2D& fallback_pan) -> QVector2D {
  const auto anchor_uv =
      WidgetPointToImageUv(anchor_widget_pos, widget_info, image_info, current_zoom, current_pan);
  if (!anchor_uv.has_value()) {
    return fallback_pan;
  }

  const float dpr = std::max(widget_info.device_pixel_ratio, 1e-4f);
  const float vw = std::max(1.0f, static_cast<float>(widget_info.widget_width) * dpr);
  const float vh = std::max(1.0f, static_cast<float>(widget_info.widget_height) * dpr);
  if (widget_info.widget_width <= 0 || widget_info.widget_height <= 0) {
    return fallback_pan;
  }

  const auto  scale = ComputeLetterboxScale(widget_info, image_info);
  const float px    = static_cast<float>(anchor_widget_pos.x()) * dpr;
  const float py    = static_cast<float>(anchor_widget_pos.y()) * dpr;
  const float ndc_x = (2.0f * px / vw) - 1.0f;
  const float ndc_y = 1.0f - (2.0f * py / vh);
  const float img_x = (2.0f * Clamp01(static_cast<float>(anchor_uv->x()))) - 1.0f;
  const float img_y = 1.0f - (2.0f * Clamp01(static_cast<float>(anchor_uv->y())));

  return QVector2D(ndc_x - (img_x * scale.x * target_zoom), ndc_y - (img_y * scale.y * target_zoom));
}

auto ViewportMapper::ComputeViewportRenderRegion(const ViewportWidgetInfo& widget_info, float zoom,
                                                 const QVector2D& pan, int base_width,
                                                 int base_height)
    -> std::optional<ViewportRenderRegion> {
  if (widget_info.widget_width <= 0 || widget_info.widget_height <= 0 || base_width <= 0 ||
      base_height <= 0) {
    return std::nullopt;
  }

  const ViewportImageInfo base_image{base_width, base_height};
  const auto              scale = ComputeLetterboxScale(widget_info, base_image);
  const float             dpr = std::max(widget_info.device_pixel_ratio, 1e-4f);
  const float             viewport_width =
      std::max(1.0f, static_cast<float>(widget_info.widget_width) * dpr);
  const float             viewport_height =
      std::max(1.0f, static_cast<float>(widget_info.widget_height) * dpr);
  const float             clamped_zoom = std::max(zoom, 1e-4f);
  const float inv_x = 1.0f / (scale.x * std::max(zoom, 1e-4f));
  const float inv_y = 1.0f / (scale.y * std::max(zoom, 1e-4f));

  float px_min = (-1.0f - pan.x()) * inv_x;
  float px_max = (1.0f - pan.x()) * inv_x;
  float py_min = (-1.0f - pan.y()) * inv_y;
  float py_max = (1.0f - pan.y()) * inv_y;

  if (px_min > px_max) std::swap(px_min, px_max);
  if (py_min > py_max) std::swap(py_min, py_max);

  px_min = std::clamp(px_min, -1.0f, 1.0f);
  px_max = std::clamp(px_max, -1.0f, 1.0f);
  py_min = std::clamp(py_min, -1.0f, 1.0f);
  py_max = std::clamp(py_max, -1.0f, 1.0f);

  const float u_min = std::clamp((px_min + 1.0f) * 0.5f, 0.0f, 1.0f);
  const float u_max = std::clamp((px_max + 1.0f) * 0.5f, 0.0f, 1.0f);
  const float v_min = std::clamp((1.0f - py_max) * 0.5f, 0.0f, 1.0f);
  const float v_max = std::clamp((1.0f - py_min) * 0.5f, 0.0f, 1.0f);

  const float roi_factor_x = std::clamp(u_max - u_min, 1e-4f, 1.0f);
  const float roi_factor_y = std::clamp(v_max - v_min, 1e-4f, 1.0f);

  const float image_ndc_min_x = pan.x() - (scale.x * clamped_zoom);
  const float image_ndc_max_x = pan.x() + (scale.x * clamped_zoom);
  const float image_ndc_min_y = pan.y() - (scale.y * clamped_zoom);
  const float image_ndc_max_y = pan.y() + (scale.y * clamped_zoom);
  const float visible_ndc_min_x = std::max(-1.0f, image_ndc_min_x);
  const float visible_ndc_max_x = std::min(1.0f, image_ndc_max_x);
  const float visible_ndc_min_y = std::max(-1.0f, image_ndc_min_y);
  const float visible_ndc_max_y = std::min(1.0f, image_ndc_max_y);
  const float visible_ndc_width = std::max(0.0f, visible_ndc_max_x - visible_ndc_min_x);
  const float visible_ndc_height = std::max(0.0f, visible_ndc_max_y - visible_ndc_min_y);

  const int roi_w = std::clamp(
      static_cast<int>(std::lround(static_cast<float>(base_width) * roi_factor_x)), 1, base_width);
  const int roi_h = std::clamp(
      static_cast<int>(std::lround(static_cast<float>(base_height) * roi_factor_y)), 1,
      base_height);

  const float center_u = std::clamp((u_min + u_max) * 0.5f, 0.0f, 1.0f);
  const float center_v = std::clamp((v_min + v_max) * 0.5f, 0.0f, 1.0f);

  ViewportRenderRegion region;
  region.x_ = std::clamp(static_cast<int>(std::lround(
                               center_u * static_cast<float>(base_width) -
                               static_cast<float>(roi_w) * 0.5f)),
                         0, std::max(0, base_width - roi_w));
  region.y_ = std::clamp(static_cast<int>(std::lround(
                               center_v * static_cast<float>(base_height) -
                               static_cast<float>(roi_h) * 0.5f)),
                         0, std::max(0, base_height - roi_h));
  region.scale_x_ = std::clamp(static_cast<float>(roi_w) / static_cast<float>(base_width), 1e-4f,
                               1.0f);
  region.scale_y_ = std::clamp(static_cast<float>(roi_h) / static_cast<float>(base_height), 1e-4f,
                               1.0f);
  region.reference_width_ = base_width;
  region.reference_height_ = base_height;
  region.target_width_ = std::clamp(
      static_cast<int>(std::ceil((visible_ndc_width * 0.5f) * viewport_width)), 1,
      static_cast<int>(std::ceil(viewport_width)));
  region.target_height_ = std::clamp(
      static_cast<int>(std::ceil((visible_ndc_height * 0.5f) * viewport_height)), 1,
      static_cast<int>(std::ceil(viewport_height)));
  return region;
}

}  // namespace alcedo
