//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/edit_viewer/mask_overlay_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

namespace alcedo {
namespace {

constexpr int kDiscSegments = 20;
constexpr int kCapSegments  = 10;
constexpr float kMinLength  = 1.0e-6f;
constexpr float kHoverHandleScale = 1.25f;

[[nodiscard]] auto IsFinitePoint(const QPointF& point) -> bool {
  return std::isfinite(point.x()) && std::isfinite(point.y());
}

[[nodiscard]] auto Premultiply(const QColor& color, float alpha_scale) -> MaskOverlayVertex {
  const float clamped = std::clamp(alpha_scale, 0.0f, 1.0f) * static_cast<float>(color.alphaF());
  MaskOverlayVertex vertex;
  vertex.r = static_cast<std::uint8_t>(
      std::lround(static_cast<float>(color.red()) * clamped));
  vertex.g = static_cast<std::uint8_t>(
      std::lround(static_cast<float>(color.green()) * clamped));
  vertex.b = static_cast<std::uint8_t>(
      std::lround(static_cast<float>(color.blue()) * clamped));
  vertex.a = static_cast<std::uint8_t>(std::lround(clamped * 255.0f));
  return vertex;
}

[[nodiscard]] auto MakeVertex(const QPointF& point, const QColor& color, float alpha_scale)
    -> MaskOverlayVertex {
  MaskOverlayVertex vertex = Premultiply(color, alpha_scale);
  vertex.x                 = static_cast<float>(point.x());
  vertex.y                 = static_cast<float>(point.y());
  return vertex;
}

void AppendTriangle(std::vector<MaskOverlayVertex>& triangles, const QPointF& a, const QPointF& b,
                    const QPointF& c, const QColor& color, float alpha_a, float alpha_b,
                    float alpha_c) {
  if (!IsFinitePoint(a) || !IsFinitePoint(b) || !IsFinitePoint(c)) {
    return;
  }
  triangles.push_back(MakeVertex(a, color, alpha_a));
  triangles.push_back(MakeVertex(b, color, alpha_b));
  triangles.push_back(MakeVertex(c, color, alpha_c));
}

void AppendTriangle(std::vector<MaskOverlayVertex>& triangles, const QPointF& a, const QPointF& b,
                    const QPointF& c, const QColor& color) {
  AppendTriangle(triangles, a, b, c, color, 1.0f, 1.0f, 1.0f);
}

[[nodiscard]] auto ExpandedClip(const QRectF& clip, float pad) -> QRectF {
  if (!clip.isValid() || clip.isEmpty()) {
    return {};
  }
  return clip.adjusted(-static_cast<qreal>(pad), -static_cast<qreal>(pad),
                       static_cast<qreal>(pad), static_cast<qreal>(pad));
}

[[nodiscard]] auto PointInsideClip(const QPointF& point, const QRectF& clip) -> bool {
  if (!clip.isValid() || clip.isEmpty()) {
    return true;
  }
  return clip.contains(point);
}

// Liang–Barsky clip of segment a→b against an axis-aligned rectangle.
[[nodiscard]] auto ClipSegment(const QPointF& a, const QPointF& b, const QRectF& clip)
    -> std::optional<std::pair<QPointF, QPointF>> {
  if (!clip.isValid() || clip.isEmpty()) {
    if (!IsFinitePoint(a) || !IsFinitePoint(b)) {
      return std::nullopt;
    }
    return std::make_pair(a, b);
  }
  const double dx = b.x() - a.x();
  const double dy = b.y() - a.y();
  double t0       = 0.0;
  double t1       = 1.0;
  const double p[4] = {-dx, dx, -dy, dy};
  const double q[4] = {a.x() - clip.left(), clip.right() - a.x(), a.y() - clip.top(),
                       clip.bottom() - a.y()};
  for (int i = 0; i < 4; ++i) {
    if (std::abs(p[i]) < 1.0e-12) {
      if (q[i] < 0.0) {
        return std::nullopt;
      }
      continue;
    }
    const double t = q[i] / p[i];
    if (p[i] < 0.0) {
      t0 = std::max(t0, t);
    } else {
      t1 = std::min(t1, t);
    }
    if (t0 > t1) {
      return std::nullopt;
    }
  }
  return std::make_pair(QPointF(a.x() + t0 * dx, a.y() + t0 * dy),
                        QPointF(a.x() + t1 * dx, a.y() + t1 * dy));
}

[[nodiscard]] auto PerpUnit(const QPointF& a, const QPointF& b) -> QPointF {
  const double dx  = b.x() - a.x();
  const double dy  = b.y() - a.y();
  const double len = std::hypot(dx, dy);
  if (len < kMinLength) {
    return {1.0, 0.0};
  }
  return {-dy / len, dx / len};
}

void AppendStrokeCoreAndFringe(std::vector<MaskOverlayVertex>& triangles, const QPointF& a,
                               const QPointF& b, float width, float aa, const QColor& color) {
  const QPointF n    = PerpUnit(a, b);
  const double half  = static_cast<double>(width) * 0.5;
  const double outer = half + static_cast<double>(std::max(aa, 0.0f));
  const QPointF c0(a.x() + n.x() * half, a.y() + n.y() * half);
  const QPointF c1(a.x() - n.x() * half, a.y() - n.y() * half);
  const QPointF c2(b.x() - n.x() * half, b.y() - n.y() * half);
  const QPointF c3(b.x() + n.x() * half, b.y() + n.y() * half);
  AppendTriangle(triangles, c0, c1, c2, color);
  AppendTriangle(triangles, c0, c2, c3, color);
  if (aa > 0.0f) {
    const QPointF o0(a.x() + n.x() * outer, a.y() + n.y() * outer);
    const QPointF o1(b.x() + n.x() * outer, b.y() + n.y() * outer);
    const QPointF o2(a.x() - n.x() * outer, a.y() - n.y() * outer);
    const QPointF o3(b.x() - n.x() * outer, b.y() - n.y() * outer);
    AppendTriangle(triangles, c0, o0, o1, color, 1.0f, 0.0f, 0.0f);
    AppendTriangle(triangles, c0, o1, c3, color, 1.0f, 0.0f, 1.0f);
    AppendTriangle(triangles, c1, c2, o3, color, 1.0f, 1.0f, 0.0f);
    AppendTriangle(triangles, c1, o3, o2, color, 1.0f, 0.0f, 0.0f);
  }
}

void AppendRoundCap(std::vector<MaskOverlayVertex>& triangles, const QPointF& center,
                    const QPointF& along, float radius, float aa, const QColor& color) {
  const double len = std::hypot(along.x(), along.y());
  if (len < kMinLength || radius <= 0.0f) {
    return;
  }
  const QPointF dir = {along.x() / len, along.y() / len};
  const QPointF n   = {-dir.y(), dir.x()};
  const double start = std::atan2(-n.y(), -n.x());
  constexpr double kPi = 3.14159265358979323846;
  for (int i = 0; i < kCapSegments; ++i) {
    const double a0 = start + (kPi * static_cast<double>(i) / kCapSegments);
    const double a1 = start + (kPi * static_cast<double>(i + 1) / kCapSegments);
    const QPointF p0(center.x() + std::cos(a0) * radius, center.y() + std::sin(a0) * radius);
    const QPointF p1(center.x() + std::cos(a1) * radius, center.y() + std::sin(a1) * radius);
    AppendTriangle(triangles, center, p0, p1, color);
    if (aa > 0.0f) {
      const float outer = radius + aa;
      const QPointF q0(center.x() + std::cos(a0) * outer, center.y() + std::sin(a0) * outer);
      const QPointF q1(center.x() + std::cos(a1) * outer, center.y() + std::sin(a1) * outer);
      AppendTriangle(triangles, p0, q0, q1, color, 1.0f, 0.0f, 0.0f);
      AppendTriangle(triangles, p0, q1, p1, color, 1.0f, 0.0f, 1.0f);
    }
  }
}

void AppendClippedStroke(std::vector<MaskOverlayVertex>& triangles, const QPointF& a,
                         const QPointF& b, float width, float aa, const QColor& color,
                         const QRectF& clip, bool round_caps) {
  const auto clipped = ClipSegment(a, b, ExpandedClip(clip, width * 0.5f + aa));
  if (!clipped) {
    return;
  }
  AppendStrokeCoreAndFringe(triangles, clipped->first, clipped->second, width, aa, color);
  if (round_caps) {
    const QPointF ab = clipped->second - clipped->first;
    AppendRoundCap(triangles, clipped->first, clipped->first - clipped->second, width * 0.5f, aa,
                   color);
    AppendRoundCap(triangles, clipped->second, ab, width * 0.5f, aa, color);
  }
}

void AppendPolylineStroke(std::vector<MaskOverlayVertex>& triangles,
                          const std::vector<QPointF>& points, bool closed, float width, float aa,
                          const QColor& color, const QRectF& clip) {
  if (points.size() < 2) {
    return;
  }
  const std::size_t count = closed ? points.size() : points.size() - 1;
  for (std::size_t i = 0; i < count; ++i) {
    AppendClippedStroke(triangles, points[i], points[(i + 1) % points.size()], width, aa, color,
                        clip, /*round_caps=*/false);
  }
}

// Dashes follow the polyline arc length, so the dash phase stays continuous
// across the adaptive tessellation vertices instead of restarting per segment.
void AppendDashedPolylineStroke(std::vector<MaskOverlayVertex>& triangles,
                                const std::vector<QPointF>& points, bool closed, float width,
                                float aa, const QColor& color, const QRectF& clip, float dash_len,
                                float gap_len) {
  if (points.size() < 2 || dash_len <= 0.0f || gap_len < 0.0f) {
    return;
  }
  const std::size_t count = closed ? points.size() : points.size() - 1;
  double            phase = 0.0;
  bool              draw  = true;
  for (std::size_t i = 0; i < count; ++i) {
    const QPointF& a  = points[i];
    const QPointF& b  = points[(i + 1) % points.size()];
    const double   dx = b.x() - a.x();
    const double   dy = b.y() - a.y();
    const double   len = std::hypot(dx, dy);
    if (len < kMinLength) {
      continue;
    }
    const double ux = dx / len;
    const double uy = dy / len;
    double       t  = 0.0;
    while (t < len - 1.0e-9) {
      const double period = draw ? static_cast<double>(dash_len) : static_cast<double>(gap_len);
      const double step   = std::min(len - t, period - phase);
      if (draw && step > 1.0e-9) {
        const QPointF p0(a.x() + ux * t, a.y() + uy * t);
        const QPointF p1(a.x() + ux * (t + step), a.y() + uy * (t + step));
        AppendClippedStroke(triangles, p0, p1, width, aa, color, clip, /*round_caps=*/false);
      }
      t += step;
      phase += step;
      if (phase >= period - 1.0e-9) {
        phase = 0.0;
        draw  = !draw;
      }
    }
  }
}

void AppendFilledDisc(std::vector<MaskOverlayVertex>& triangles, const QPointF& center,
                      float radius, float aa, const QColor& color, const QRectF& clip) {
  if (radius <= 0.0f || !IsFinitePoint(center) ||
      !PointInsideClip(center, ExpandedClip(clip, radius + aa))) {
    return;
  }
  for (int i = 0; i < kDiscSegments; ++i) {
    const float a0 =
        (static_cast<float>(i) / static_cast<float>(kDiscSegments)) * 6.28318530718f;
    const float a1 =
        (static_cast<float>(i + 1) / static_cast<float>(kDiscSegments)) * 6.28318530718f;
    const QPointF p0(center.x() + std::cos(a0) * radius, center.y() + std::sin(a0) * radius);
    const QPointF p1(center.x() + std::cos(a1) * radius, center.y() + std::sin(a1) * radius);
    AppendTriangle(triangles, center, p0, p1, color);
    if (aa > 0.0f) {
      const float outer = radius + aa;
      const QPointF q0(center.x() + std::cos(a0) * outer, center.y() + std::sin(a0) * outer);
      const QPointF q1(center.x() + std::cos(a1) * outer, center.y() + std::sin(a1) * outer);
      AppendTriangle(triangles, p0, q0, q1, color, 1.0f, 0.0f, 0.0f);
      AppendTriangle(triangles, p0, q1, p1, color, 1.0f, 0.0f, 1.0f);
    }
  }
}

void AppendRing(std::vector<MaskOverlayVertex>& triangles, const QPointF& center, float inner_r,
                float outer_r, float aa, const QColor& color, const QRectF& clip) {
  if (outer_r <= inner_r || !IsFinitePoint(center) ||
      !PointInsideClip(center, ExpandedClip(clip, outer_r + aa))) {
    return;
  }
  for (int i = 0; i < kDiscSegments; ++i) {
    const float a0 =
        (static_cast<float>(i) / static_cast<float>(kDiscSegments)) * 6.28318530718f;
    const float a1 =
        (static_cast<float>(i + 1) / static_cast<float>(kDiscSegments)) * 6.28318530718f;
    const QPointF o0(center.x() + std::cos(a0) * outer_r, center.y() + std::sin(a0) * outer_r);
    const QPointF o1(center.x() + std::cos(a1) * outer_r, center.y() + std::sin(a1) * outer_r);
    const QPointF i0(center.x() + std::cos(a0) * inner_r, center.y() + std::sin(a0) * inner_r);
    const QPointF i1(center.x() + std::cos(a1) * inner_r, center.y() + std::sin(a1) * inner_r);
    AppendTriangle(triangles, o0, i0, i1, color);
    AppendTriangle(triangles, o0, i1, o1, color);
    if (aa > 0.0f) {
      const float fringe = outer_r + aa;
      const QPointF f0(center.x() + std::cos(a0) * fringe, center.y() + std::sin(a0) * fringe);
      const QPointF f1(center.x() + std::cos(a1) * fringe, center.y() + std::sin(a1) * fringe);
      AppendTriangle(triangles, o0, f0, f1, color, 1.0f, 0.0f, 0.0f);
      AppendTriangle(triangles, o0, f1, o1, color, 1.0f, 0.0f, 1.0f);
    }
  }
}

void AppendHollowCircle(std::vector<MaskOverlayVertex>& triangles, const QPointF& center,
                        float radius, float width, float aa, const QColor& color,
                        const QRectF& clip) {
  if (radius <= 0.0f) {
    return;
  }
  const float half  = width * 0.5f;
  const float inner = std::max(0.0f, radius - half);
  const float outer = radius + half;
  AppendRing(triangles, center, inner, outer, aa, color, clip);
}

void AppendDashedHollowCircle(std::vector<MaskOverlayVertex>& triangles, const QPointF& center,
                              float radius, float width, float aa, const QColor& color,
                              const QRectF& clip, float dash_len, float gap_len) {
  if (radius <= 0.0f || !IsFinitePoint(center)) {
    return;
  }
  const int segments = std::clamp(static_cast<int>(std::ceil(radius * 2.0f)), 24, 128);
  std::vector<QPointF> ring;
  ring.reserve(static_cast<std::size_t>(segments));
  for (int i = 0; i < segments; ++i) {
    const float a = (static_cast<float>(i) / static_cast<float>(segments)) * 6.28318530718f;
    ring.emplace_back(center.x() + std::cos(a) * radius, center.y() + std::sin(a) * radius);
  }
  AppendDashedPolylineStroke(triangles, ring, /*closed=*/true, width, aa, color, clip, dash_len,
                             gap_len);
}

}  // namespace

auto DefaultMaskOverlayStyle() -> MaskOverlayStyle { return {}; }

auto BuildMaskOverlaySceneGeometry(const MaskOverlayDisplay& display, const MaskOverlayStyle& style)
    -> MaskOverlaySceneGeometry {
  MaskOverlaySceneGeometry scene;
  scene.coverage_fill_vertex_count       = 0;
  scene.settled_stroke_path_vertex_count = 0;
  if (display.mode == MaskOverlayMode::Hidden ||
      display.source_kind == MaskOverlaySourceKind::None) {
    return scene;
  }

  const float handle_r = style.handle_radius_logical_px;
  const float outline  = style.handle_outline_width_logical_px;
  const float stroke_w = style.stroke_width_logical_px;
  const float aa       = style.antialias_width_logical_px;
  const float guide_outer = style.guide_outer_width_logical_px;
  const float guide_inner = style.guide_inner_width_logical_px;
  const float grip_outer  = style.grip_outer_width_logical_px;
  const float grip_inner  = style.grip_inner_width_logical_px;
  const QRectF& clip   = display.clip_rect;

  const auto handle_radius = [&](MaskOverlayHandleId id) {
    if (id == display.hovered_handle || id == display.active_handle) {
      return handle_r * kHoverHandleScale;
    }
    return handle_r;
  };
  const auto append_frame_segment = [&](const QPointF& a, const QPointF& b) {
    AppendClippedStroke(scene.selected_guides, a, b, guide_outer, aa, style.control_fill, clip,
                        /*round_caps=*/false);
    AppendClippedStroke(scene.selected_guides, a, b, guide_inner, aa, style.control_outline, clip,
                        /*round_caps=*/false);
    ++scene.selected_guide_segment_count;
  };

  for (std::size_t handle_index = 0; handle_index < display.handles.size(); ++handle_index) {
    const auto& handle = display.handles[handle_index];
    if (handle.id == MaskOverlayHandleId::None || !IsFinitePoint(handle.item)) {
      continue;
    }
    if (handle.shape == MaskOverlayHandleShape::CornerTick) {
      // Brush Move frame anchor: two short segments from the corner along the
      // adjacent frame edges. Edge directions come from the neighbouring
      // corner handles, so the tick follows the rotated frame.
      const auto* prev = &handle;
      const auto* next = &handle;
      for (std::size_t i = 1; i <= display.handles.size(); ++i) {
        const auto& candidate =
            display.handles[(handle_index + display.handles.size() - i) % display.handles.size()];
        if (candidate.shape == MaskOverlayHandleShape::CornerTick &&
            IsFinitePoint(candidate.item)) {
          prev = &candidate;
          break;
        }
      }
      for (std::size_t i = 1; i <= display.handles.size(); ++i) {
        const auto& candidate = display.handles[(handle_index + i) % display.handles.size()];
        if (candidate.shape == MaskOverlayHandleShape::CornerTick &&
            IsFinitePoint(candidate.item)) {
          next = &candidate;
          break;
        }
      }
      const float tick_len = handle_radius(handle.id) * 2.4f;
      for (const auto* neighbour : {prev, next}) {
        if (neighbour == &handle) {
          continue;
        }
        const QPointF delta = neighbour->item - handle.item;
        const double  len   = std::hypot(delta.x(), delta.y());
        if (len < kMinLength) {
          continue;
        }
        const QPointF end(handle.item.x() + delta.x() / len * tick_len,
                          handle.item.y() + delta.y() / len * tick_len);
        AppendClippedStroke(scene.edge_grips, handle.item, end, grip_outer, aa,
                            style.control_fill, clip, /*round_caps=*/false);
        AppendClippedStroke(scene.edge_grips, handle.item, end, grip_inner, aa,
                            style.control_outline, clip, /*round_caps=*/false);
      }
      ++scene.handle_count;
      continue;
    }
    const float radius = handle_radius(handle.id);
    if (handle.shape == MaskOverlayHandleShape::Ring) {
      AppendHollowCircle(scene.handle_outline, handle.item, radius + outline * 0.5f, outline, aa,
                         style.control_outline, clip);
      AppendHollowCircle(scene.handle_fill, handle.item, radius, outline, aa, style.control_fill,
                         clip);
    } else {
      AppendRing(scene.handle_outline, handle.item, radius, radius + outline, aa,
                 style.control_outline, clip);
      AppendFilledDisc(scene.handle_fill, handle.item, radius, 0.0f, style.control_fill, clip);
    }
    ++scene.handle_count;
  }

  if (display.move_frame_visible) {
    // Brush Move frame: closed outline through the ordered CornerTick handles.
    std::vector<QPointF> frame;
    for (const auto& handle : display.handles) {
      if (handle.shape == MaskOverlayHandleShape::CornerTick && IsFinitePoint(handle.item)) {
        frame.push_back(handle.item);
      }
    }
    for (std::size_t i = 0; i + 1 < frame.size(); ++i) {
      append_frame_segment(frame[i], frame[i + 1]);
    }
    if (frame.size() > 2) {
      append_frame_segment(frame.back(), frame.front());
    }
  }

  for (const auto& segment : display.connectors) {
    AppendClippedStroke(scene.connectors, segment.first, segment.second, stroke_w, aa,
                        style.control_fill, clip, /*round_caps=*/true);
  }

  if (display.cursor_visible && display.cursor_radius_logical_px > 0.0f &&
      IsFinitePoint(display.cursor_center)) {
    if (display.cursor_dashed) {
      AppendDashedHollowCircle(scene.cursor, display.cursor_center,
                              display.cursor_radius_logical_px, stroke_w, aa, style.control_fill,
                              clip, kMaskOverlayDashLengthLogicalPx,
                              kMaskOverlayDashGapLogicalPx);
    } else {
      AppendHollowCircle(scene.cursor, display.cursor_center, display.cursor_radius_logical_px,
                         stroke_w, aa, style.control_fill, clip);
    }
  }

  if (display.mode == MaskOverlayMode::Creating) {
    AppendPolylineStroke(scene.creation_guides, display.creation_path, /*closed=*/false, stroke_w,
                         aa, style.inactive, clip);
    AppendPolylineStroke(scene.creation_guides, display.creation_outline, /*closed=*/true, stroke_w,
                         aa, style.inactive, clip);
    for (const auto& segment : display.creation_guides) {
      AppendClippedStroke(scene.creation_guides, segment.first, segment.second, stroke_w, aa,
                          style.inactive, clip, /*round_caps=*/false);
    }
  }

  auto append_dual = [&](std::vector<MaskOverlayVertex>& triangles, const QPointF& a,
                          const QPointF& b, float outer_w, float inner_w) {
    AppendClippedStroke(triangles, a, b, outer_w, aa, style.control_fill, clip,
                        /*round_caps=*/false);
    AppendClippedStroke(triangles, a, b, inner_w, aa, style.control_outline, clip,
                        /*round_caps=*/false);
  };

  for (const auto& contour : display.selected_contours) {
    if (contour.points.size() < 2) {
      continue;
    }
    const std::size_t count = contour.points.size();
    if (contour.dashed) {
      AppendDashedPolylineStroke(scene.selected_guides, contour.points, /*closed=*/true,
                                 guide_outer, aa, style.control_fill, clip,
                                 kMaskOverlayDashLengthLogicalPx, kMaskOverlayDashGapLogicalPx);
      AppendDashedPolylineStroke(scene.selected_guides, contour.points, /*closed=*/true,
                                 guide_inner, aa, style.control_outline, clip,
                                 kMaskOverlayDashLengthLogicalPx, kMaskOverlayDashGapLogicalPx);
      scene.selected_guide_segment_count += static_cast<int>(count);
      continue;
    }
    for (std::size_t i = 0; i < count; ++i) {
      append_dual(scene.selected_guides, contour.points[i], contour.points[(i + 1) % count],
                  guide_outer, guide_inner);
      ++scene.selected_guide_segment_count;
    }
  }
  for (const auto& guide : display.selected_guides) {
    append_dual(scene.selected_guides, guide.a, guide.b, guide_outer, guide_inner);
    ++scene.selected_guide_segment_count;
  }
  for (const auto& grip : display.edge_grips) {
    append_dual(scene.edge_grips, grip.first, grip.second, grip_outer, grip_inner);
  }
  scene.closed_polygon_edge_count         = 0;
  scene.coverage_fill_vertex_count       = 0;
  scene.settled_stroke_path_vertex_count = 0;

  return scene;
}

}  // namespace alcedo
