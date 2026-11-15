//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/plot_pane/plot_primitives.hpp"

#include <algorithm>
#include <cmath>

#include "ui/edit_viewer/overlay_triangle_primitives.hpp"

namespace alcedo::ui::plot_primitives {
namespace {

constexpr int   kRingSegments = 32;
constexpr float kTwoPi        = 6.28318530718f;

// Core width of a stroke whose antialiased edges add kAntialiasWidth to the covered width.
auto            CoreWidth(float width) -> float { return std::max(0.0f, width - kAntialiasWidth); }

}  // namespace

void AppendStroke(std::vector<MaskOverlayVertex>& triangles, const std::vector<QPointF>& points,
                  float width, const QColor& color, const QRectF& clip) {
  overlay_triangles::AppendPolylineStroke(triangles, points, /*closed=*/false, CoreWidth(width),
                                          kAntialiasWidth, color, clip);
}

void AppendDashedStroke(std::vector<MaskOverlayVertex>& triangles,
                        const std::vector<QPointF>& points, float width, float dash_length,
                        float gap_length, const QColor& color, const QRectF& clip) {
  overlay_triangles::AppendDashedPolylineStroke(triangles, points, /*closed=*/false,
                                                CoreWidth(width), kAntialiasWidth, color, clip,
                                                dash_length, gap_length);
}

void AppendRing(std::vector<MaskOverlayVertex>& triangles, const QPointF& center, float radius,
                float width, const QColor& color, const QRectF& clip) {
  const float half_aa = kAntialiasWidth * 0.5f;
  const float outer   = radius - half_aa;
  const float inner   = std::max(0.0f, radius - width + half_aa);
  if (outer <= 0.0f || !overlay_triangles::IsFinitePoint(center) ||
      !overlay_triangles::PointInsideClip(center, overlay_triangles::ExpandedClip(clip, radius))) {
    return;
  }
  // Core and outer edge.
  overlay_triangles::AppendRing(triangles, center, std::min(inner, outer), outer, kAntialiasWidth,
                                color, clip);
  // Inner edge: alpha falls from the core to the center side.
  const float fringe = std::max(0.0f, std::min(inner, outer) - kAntialiasWidth);
  for (int i = 0; i < kRingSegments; ++i) {
    const float   a0 = (static_cast<float>(i) / kRingSegments) * kTwoPi;
    const float   a1 = (static_cast<float>(i + 1) / kRingSegments) * kTwoPi;
    const float   r  = std::min(inner, outer);
    const QPointF c0(center.x() + std::cos(a0) * r, center.y() + std::sin(a0) * r);
    const QPointF c1(center.x() + std::cos(a1) * r, center.y() + std::sin(a1) * r);
    const QPointF f0(center.x() + std::cos(a0) * fringe, center.y() + std::sin(a0) * fringe);
    const QPointF f1(center.x() + std::cos(a1) * fringe, center.y() + std::sin(a1) * fringe);
    overlay_triangles::AppendTriangle(triangles, c0, f0, f1, color, 1.0f, 0.0f, 0.0f);
    overlay_triangles::AppendTriangle(triangles, c0, f1, c1, color, 1.0f, 0.0f, 1.0f);
  }
}

void AppendDisc(std::vector<MaskOverlayVertex>& triangles, const QPointF& center, float radius,
                const QColor& color, const QRectF& clip) {
  const float half_aa = kAntialiasWidth * 0.5f;
  overlay_triangles::AppendFilledDisc(triangles, center, std::max(0.0f, radius - half_aa),
                                      kAntialiasWidth, color, clip);
}

void AppendRect(std::vector<MaskOverlayVertex>& triangles, const QRectF& rect,
                const QColor& color) {
  if (rect.isEmpty()) {
    return;
  }
  overlay_triangles::AppendTriangle(triangles, rect.topLeft(), rect.topRight(), rect.bottomRight(),
                                    color);
  overlay_triangles::AppendTriangle(triangles, rect.topLeft(), rect.bottomRight(),
                                    rect.bottomLeft(), color);
}

auto TintChroma(const QColor& tint) -> std::array<float, 3> {
  const std::array<float, 3> rgb = {static_cast<float>(tint.redF()),
                                    static_cast<float>(tint.greenF()),
                                    static_cast<float>(tint.blueF())};
  const auto [minimum, maximum]  = std::minmax_element(rgb.begin(), rgb.end());
  const float range              = *maximum - *minimum;
  if (range <= 0.001f) {
    return {1.0f, 1.0f, 1.0f};
  }
  return {(rgb[0] - *minimum) / range, (rgb[1] - *minimum) / range, (rgb[2] - *minimum) / range};
}

auto AdditiveChannelChroma(const std::array<QColor, 3>& tints)
    -> std::array<std::array<float, 3>, 3> {
  std::array<std::array<float, 3>, 3> chroma = {TintChroma(tints[0]), TintChroma(tints[1]),
                                                TintChroma(tints[2])};
  for (size_t component = 0; component < 3; ++component) {
    const float sum = chroma[0][component] + chroma[1][component] + chroma[2][component];
    for (auto& channel : chroma) {
      channel[component] = sum > 0.001f ? channel[component] / sum : 1.0f / 3.0f;
    }
  }
  return chroma;
}

auto ScaledChromaColor(const std::array<float, 3>& chroma, float strength) -> QColor {
  return QColor::fromRgbF(std::clamp(chroma[0] * strength, 0.0f, 1.0f),
                          std::clamp(chroma[1] * strength, 0.0f, 1.0f),
                          std::clamp(chroma[2] * strength, 0.0f, 1.0f));
}

auto AdditiveVertex(const QPointF& point, const QColor& color, float scale) -> MaskOverlayVertex {
  const float       s = std::clamp(scale, 0.0f, 1.0f);
  MaskOverlayVertex vertex;
  vertex.x = static_cast<float>(point.x());
  vertex.y = static_cast<float>(point.y());
  vertex.r = static_cast<std::uint8_t>(std::lround(static_cast<float>(color.red()) * s));
  vertex.g = static_cast<std::uint8_t>(std::lround(static_cast<float>(color.green()) * s));
  vertex.b = static_cast<std::uint8_t>(std::lround(static_cast<float>(color.blue()) * s));
  vertex.a = 0;
  return vertex;
}

}  // namespace alcedo::ui::plot_primitives
