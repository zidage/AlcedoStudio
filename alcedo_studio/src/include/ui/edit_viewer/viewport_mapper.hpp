//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <optional>

#include <QPointF>
#include <QVector2D>

#include "ui/edit_viewer/frame_sink.hpp"

namespace alcedo {

struct ViewportWidgetInfo {
  int   widget_width       = 0;
  int   widget_height      = 0;
  float device_pixel_ratio = 1.0f;
};

struct ViewportImageInfo {
  int image_width  = 0;
  int image_height = 0;
};

struct LetterboxScale {
  float x = 1.0f;
  float y = 1.0f;
};

class ViewportMapper {
 public:
  static auto Clamp01(float value) -> float;

  static auto ComputeLetterboxScale(const ViewportWidgetInfo& widget_info,
                                    const ViewportImageInfo& image_info) -> LetterboxScale;

  static auto WidgetPointToImageUv(const QPointF& widget_pos, const ViewportWidgetInfo& widget_info,
                                   const ViewportImageInfo& image_info, float zoom,
                                   const QVector2D& pan) -> std::optional<QPointF>;

  static auto ImageUvToWidgetPoint(const QPointF& uv, const ViewportWidgetInfo& widget_info,
                                   const ViewportImageInfo& image_info, float zoom,
                                   const QVector2D& pan, bool clamp_uv = true)
      -> std::optional<QPointF>;

  static auto ClampPanForZoom(const ViewportWidgetInfo& widget_info,
                              const ViewportImageInfo& image_info, float zoom,
                              const QVector2D& pan, float min_zoom, float max_zoom)
      -> QVector2D;

  static auto ComputeAnchoredPan(const QPointF& anchor_widget_pos,
                                 const ViewportWidgetInfo& widget_info,
                                 const ViewportImageInfo& image_info, float current_zoom,
                                 const QVector2D& current_pan, float target_zoom,
                                 const QVector2D& fallback_pan) -> QVector2D;

  static auto ComputeViewportRenderRegion(const ViewportWidgetInfo& widget_info, float zoom,
                                          const QVector2D& pan, int base_width, int base_height)
      -> std::optional<ViewportRenderRegion>;
};

}  // namespace alcedo
