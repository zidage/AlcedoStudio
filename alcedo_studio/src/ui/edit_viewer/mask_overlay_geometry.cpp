//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/edit_viewer/mask_overlay_geometry.hpp"

#include <algorithm>
#include <cmath>

#include "ui/edit_viewer/overlay_triangle_primitives.hpp"

namespace alcedo {

using namespace overlay_triangles;

namespace {

constexpr float kHoverHandleScale = 1.25f;

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
        if (len < kOverlayMinLength) {
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
    if (guide.dashed) {
      const std::vector<QPointF> points{guide.a, guide.b};
      AppendDashedPolylineStroke(scene.selected_guides, points, /*closed=*/false, guide_outer, aa,
                                 style.control_fill, clip, kMaskOverlayDashLengthLogicalPx,
                                 kMaskOverlayDashGapLogicalPx);
      AppendDashedPolylineStroke(scene.selected_guides, points, /*closed=*/false, guide_inner, aa,
                                 style.control_outline, clip, kMaskOverlayDashLengthLogicalPx,
                                 kMaskOverlayDashGapLogicalPx);
    } else {
      append_dual(scene.selected_guides, guide.a, guide.b, guide_outer, guide_inner);
    }
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
