//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/edit_viewer/mask_overlay_layout.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace alcedo {
namespace {

constexpr float kPi          = 3.14159265358979323846f;
constexpr float kMinRadius   = 1.0e-5f;
constexpr float kHandleMerge = 2.0f;

[[nodiscard]] auto IsFiniteVector(Vector2 point) -> bool {
  return std::isfinite(point.x) && std::isfinite(point.y);
}

[[nodiscard]] auto WidgetClip(const MaskEditViewMapping& mapping) -> QRectF {
  if (mapping.widget.widget_width <= 0 || mapping.widget.widget_height <= 0) {
    return {};
  }
  return QRectF(0.0, 0.0, mapping.widget.widget_width, mapping.widget.widget_height);
}

[[nodiscard]] auto EffectiveClip(const MaskEditViewMapping& mapping, const QRectF& clip) -> QRectF {
  if (clip.isValid() && !clip.isEmpty()) {
    return clip;
  }
  return WidgetClip(mapping);
}

[[nodiscard]] auto ItemDistance(const QPointF& a, const QPointF& b) -> float {
  return static_cast<float>(std::hypot(a.x() - b.x(), a.y() - b.y()));
}

void AppendHandle(MaskOverlayDisplay& display, MaskOverlayHandleId id, const QPointF& item,
                  const QPointF& origin, float merge_px) {
  if (id != MaskOverlayHandleId::RadialCenter && id != MaskOverlayHandleId::LinearOrigin &&
      id != MaskOverlayHandleId::BrushMove && ItemDistance(item, origin) < merge_px) {
    return;
  }
  for (const auto& existing : display.handles) {
    if (ItemDistance(existing.item, item) < merge_px) {
      return;
    }
  }
  display.handles.push_back(MaskOverlayHandle{id, item});
}

void AppendConnector(MaskOverlayDisplay& display, const QPointF& a, const QPointF& b) {
  if (ItemDistance(a, b) < kMinRadius) {
    return;
  }
  display.connectors.emplace_back(a, b);
}

[[nodiscard]] auto MapRho(const MaskEditViewMapping& mapping, const RadialMaskSource& source,
                          float rho, float theta) -> std::optional<QPointF> {
  return MapNormalizedMaskPointToItem(mapping, RadialNormalizedPoint(source, rho, theta));
}

[[nodiscard]] auto InnerRho(const RadialMaskSource& source) -> float {
  return std::max(0.0f, 1.0f - source.inner_feather);
}

[[nodiscard]] auto OuterRho(const RadialMaskSource& source) -> float {
  return 1.0f + std::max(0.0f, source.outer_feather);
}

[[nodiscard]] auto OffsetAlongItem(const QPointF& origin, const QPointF& along, float offset_px)
    -> std::optional<QPointF> {
  const QPointF delta = along - origin;
  const float len     = ItemDistance(along, origin);
  if (len < kMinRadius) {
    return std::nullopt;
  }
  const float scale = offset_px / len;
  return QPointF(origin.x() + delta.x() * static_cast<qreal>(scale),
                 origin.y() + delta.y() * static_cast<qreal>(scale));
}

[[nodiscard]] auto LinearNormal(const LinearGradientMaskSource& source) -> Vector2 {
  const float length = std::hypot(source.normal_x, source.normal_y);
  if (!std::isfinite(length) || length < kMinRadius) {
    return {0.0f, 1.0f};
  }
  return {source.normal_x / length, source.normal_y / length};
}

[[nodiscard]] auto LinearLocus(const LinearGradientMaskSource& source, float signed_distance)
    -> Vector2 {
  const Vector2 n = LinearNormal(source);
  return Vector2{source.origin_x + n.x * signed_distance, source.origin_y + n.y * signed_distance};
}

[[nodiscard]] auto PerpNormalized(Vector2 n) -> Vector2 { return {-n.y, n.x}; }

void PopulateRadialHandles(MaskOverlayDisplay& display, const MaskEditViewMapping& mapping,
                           const RadialMaskSource& source, const MaskOverlayStyle& style,
                           const QRectF& clip) {
  const auto center = MapNormalizedMaskPointToItem(
      mapping, Vector2{source.center_x, source.center_y});
  if (!center) {
    return;
  }
  display.handles.push_back(MaskOverlayHandle{MaskOverlayHandleId::RadialCenter, *center});

  const auto major = MapRho(mapping, source, 1.0f, 0.0f);
  const auto minor = MapRho(mapping, source, 1.0f, kPi * 0.5f);
  if (major) {
    AppendHandle(display, MaskOverlayHandleId::RadialMajor, *major, *center, kHandleMerge);
    AppendConnector(display, *center, *major);
    if (const auto rotate =
            OffsetAlongItem(*center, *major, style.rotate_handle_offset_logical_px)) {
      if (clip.isEmpty() || clip.contains(*rotate)) {
        AppendHandle(display, MaskOverlayHandleId::RadialRotate, *rotate, *center, kHandleMerge);
        AppendConnector(display, *major, *rotate);
      }
    }
  }
  if (minor) {
    AppendHandle(display, MaskOverlayHandleId::RadialMinor, *minor, *center, kHandleMerge);
    AppendConnector(display, *center, *minor);
  }

  const float inner = InnerRho(source);
  const float outer = OuterRho(source);
  if (std::fabs(inner - 1.0f) > 1.0e-3f && inner > kMinRadius) {
    if (const auto inner_item = MapRho(mapping, source, inner, 0.0f)) {
      AppendHandle(display, MaskOverlayHandleId::RadialInnerFeather, *inner_item, *center,
                   kHandleMerge);
      AppendConnector(display, *center, *inner_item);
    }
  }
  if (std::fabs(outer - 1.0f) > 1.0e-3f) {
    if (const auto outer_item = MapRho(mapping, source, outer, 0.0f)) {
      AppendHandle(display, MaskOverlayHandleId::RadialOuterFeather, *outer_item, *center,
                   kHandleMerge);
      AppendConnector(display, *center, *outer_item);
    }
  }
}

void PopulateLinearHandles(MaskOverlayDisplay& display, const MaskEditViewMapping& mapping,
                           const LinearGradientMaskSource& source, const MaskOverlayStyle& style,
                           const QRectF& clip) {
  const auto origin = MapNormalizedMaskPointToItem(
      mapping, Vector2{source.origin_x, source.origin_y});
  if (!origin) {
    return;
  }
  display.handles.push_back(MaskOverlayHandle{MaskOverlayHandleId::LinearOrigin, *origin});

  const float half = std::max(source.transition_distance, kMinRadius) * 0.5f;
  const auto start = MapNormalizedMaskPointToItem(mapping, LinearLocus(source, -half));
  const auto end   = MapNormalizedMaskPointToItem(mapping, LinearLocus(source, half));
  if (start) {
    AppendHandle(display, MaskOverlayHandleId::LinearStartBoundary, *start, *origin, kHandleMerge);
    AppendConnector(display, *origin, *start);
  }
  if (end) {
    AppendHandle(display, MaskOverlayHandleId::LinearEndBoundary, *end, *origin, kHandleMerge);
    AppendConnector(display, *origin, *end);
    if (const auto direction =
            OffsetAlongItem(*origin, *end, style.rotate_handle_offset_logical_px)) {
      if (clip.isEmpty() || clip.contains(*direction)) {
        AppendHandle(display, MaskOverlayHandleId::LinearDirection, *direction, *origin,
                     kHandleMerge);
        AppendConnector(display, *end, *direction);
      }
    }
  }
}

[[nodiscard]] auto FiniteRadii(const RadialMaskSource& source) -> bool {
  return std::isfinite(source.major_radius) && std::isfinite(source.minor_radius) &&
         source.major_radius > kMinRadius && source.minor_radius > kMinRadius &&
         std::isfinite(source.center_x) && std::isfinite(source.center_y) &&
         std::isfinite(source.rotation);
}

}  // namespace

auto MapNormalizedMaskPointToItem(const MaskEditViewMapping& mapping, Vector2 normalized)
    -> std::optional<QPointF> {
  if (!MaskEditGeometry::IsValid(mapping) || !IsFiniteVector(normalized)) {
    return std::nullopt;
  }
  const Vector2 reference = MaskEditGeometry::ReferencePixelsFromNormalized(
      normalized, mapping.geometry.full_reference_extent);
  return MaskEditGeometry::MapReferenceToItem(mapping, reference);
}

auto RadialNormalizedPoint(const RadialMaskSource& source, float rho, float theta_radians)
    -> Vector2 {
  const float c = std::cos(source.rotation);
  const float s = std::sin(source.rotation);
  const float u = rho * std::cos(theta_radians);
  const float v = rho * std::sin(theta_radians);
  const float dx = c * (u * source.major_radius) - s * (v * source.minor_radius);
  const float dy = s * (u * source.major_radius) + c * (v * source.minor_radius);
  return Vector2{source.center_x + dx, source.center_y + dy};
}

auto TessellateRadialBoundaryItemPolyline(const MaskEditViewMapping& mapping,
                                          const RadialMaskSource& source, float rho,
                                          const QRectF& clip) -> std::vector<QPointF> {
  (void)clip;
  if (!FiniteRadii(source) || !std::isfinite(rho) || rho < 0.0f) {
    return {};
  }

  auto map_theta = [&](float theta) -> std::optional<QPointF> {
    return MapRho(mapping, source, rho, theta);
  };
  auto wrap_mid = [](float a, float b) -> float {
    if (b < a) {
      return std::fmod(0.5f * (a + b + 2.0f * kPi), 2.0f * kPi);
    }
    return 0.5f * (a + b);
  };
  auto chord_error = [&](float t0, float t1, const QPointF& p0, const QPointF& p1) -> float {
    const auto true_item = map_theta(wrap_mid(t0, t1));
    if (!true_item) {
      return 0.0f;
    }
    const QPointF chord(0.5 * (p0.x() + p1.x()), 0.5 * (p0.y() + p1.y()));
    return ItemDistance(*true_item, chord);
  };

  constexpr int kSeed = 32;
  std::vector<float> thetas;
  std::vector<QPointF> points;
  thetas.reserve(kSeed);
  points.reserve(kSeed);
  for (int i = 0; i < kSeed; ++i) {
    const float theta = (static_cast<float>(i) / static_cast<float>(kSeed)) * (2.0f * kPi);
    const auto item   = map_theta(theta);
    if (!item) {
      return {};
    }
    thetas.push_back(theta);
    points.push_back(*item);
  }

  bool grew = true;
  while (grew && static_cast<int>(points.size()) < kMaskOverlayMaxEllipseVertices) {
    grew = false;
    std::vector<float> next_thetas;
    std::vector<QPointF> next_points;
    next_thetas.reserve(points.size() * 2);
    next_points.reserve(points.size() * 2);
    for (std::size_t i = 0; i < points.size(); ++i) {
      next_thetas.push_back(thetas[i]);
      next_points.push_back(points[i]);
      const std::size_t j = (i + 1) % points.size();
      if (static_cast<int>(next_points.size()) >= kMaskOverlayMaxEllipseVertices) {
        continue;
      }
      if (chord_error(thetas[i], thetas[j], points[i], points[j]) <=
          kMaskOverlayMaxChordDeviationLogicalPx) {
        continue;
      }
      const float mid_theta = wrap_mid(thetas[i], thetas[j]);
      const auto mid_item   = map_theta(mid_theta);
      if (!mid_item) {
        continue;
      }
      next_thetas.push_back(mid_theta);
      next_points.push_back(*mid_item);
      grew = true;
    }
    thetas.swap(next_thetas);
    points.swap(next_points);
  }

  if (points.size() < 3) {
    return {};
  }
  return points;
}

auto MakeBrushExistingOverlayDisplay(const MaskEditViewMapping& mapping,
                                     Vector2 placement_translation, const QRectF& clip)
    -> MaskOverlayDisplay {
  MaskOverlayDisplay display;
  display.mode        = MaskOverlayMode::Existing;
  display.source_kind = MaskOverlaySourceKind::Brush;
  display.clip_rect   = EffectiveClip(mapping, clip);
  const auto item     = MaskEditGeometry::MapReferenceToItem(mapping, placement_translation);
  if (!item) {
    display.mode        = MaskOverlayMode::Hidden;
    display.source_kind = MaskOverlaySourceKind::None;
    return display;
  }
  display.handles.push_back(MaskOverlayHandle{MaskOverlayHandleId::BrushMove, *item});
  return display;
}

auto MakeRadialExistingOverlayDisplay(const MaskEditViewMapping& mapping,
                                      const RadialMaskSource& source, const MaskOverlayStyle& style,
                                      const QRectF& clip) -> MaskOverlayDisplay {
  MaskOverlayDisplay display;
  if (!FiniteRadii(source)) {
    return display;
  }
  display.mode        = MaskOverlayMode::Existing;
  display.source_kind = MaskOverlaySourceKind::Radial;
  display.clip_rect   = EffectiveClip(mapping, clip);
  PopulateRadialHandles(display, mapping, source, style, display.clip_rect);
  if (display.handles.empty()) {
    display.mode        = MaskOverlayMode::Hidden;
    display.source_kind = MaskOverlaySourceKind::None;
  }
  return display;
}

auto MakeLinearExistingOverlayDisplay(const MaskEditViewMapping& mapping,
                                      const LinearGradientMaskSource& source,
                                      const MaskOverlayStyle& style, const QRectF& clip)
    -> MaskOverlayDisplay {
  MaskOverlayDisplay display;
  if (!std::isfinite(source.origin_x) || !std::isfinite(source.origin_y) ||
      !std::isfinite(source.transition_distance)) {
    return display;
  }
  display.mode        = MaskOverlayMode::Existing;
  display.source_kind = MaskOverlaySourceKind::LinearGradient;
  display.clip_rect   = EffectiveClip(mapping, clip);
  PopulateLinearHandles(display, mapping, source, style, display.clip_rect);
  if (display.handles.empty()) {
    display.mode        = MaskOverlayMode::Hidden;
    display.source_kind = MaskOverlaySourceKind::None;
  }
  return display;
}

auto MakeBrushCreatingOverlayDisplay(const std::vector<QPointF>& item_path, QPointF cursor_item,
                                     float cursor_radius_logical_px, const QRectF& clip)
    -> MaskOverlayDisplay {
  MaskOverlayDisplay display;
  display.mode                     = MaskOverlayMode::Creating;
  display.source_kind              = MaskOverlaySourceKind::Brush;
  display.clip_rect                = clip;
  display.creation_path            = item_path;
  display.cursor_visible           = std::isfinite(cursor_item.x()) && std::isfinite(cursor_item.y()) &&
                           cursor_radius_logical_px > 0.0f;
  display.cursor_center            = cursor_item;
  display.cursor_radius_logical_px = cursor_radius_logical_px;
  return display;
}

auto MakeRadialCreatingOverlayDisplay(const MaskEditViewMapping& mapping,
                                      const RadialMaskSource& source, const MaskOverlayStyle& style,
                                      const QRectF& clip) -> MaskOverlayDisplay {
  auto display          = MakeRadialExistingOverlayDisplay(mapping, source, style, clip);
  if (display.mode == MaskOverlayMode::Hidden) {
    return display;
  }
  display.mode             = MaskOverlayMode::Creating;
  display.creation_outline = TessellateRadialBoundaryItemPolyline(mapping, source, 1.0f,
                                                                  display.clip_rect);
  return display;
}

auto MakeLinearCreatingOverlayDisplay(const MaskEditViewMapping& mapping,
                                      const LinearGradientMaskSource& source,
                                      const MaskOverlayStyle& style, const QRectF& clip)
    -> MaskOverlayDisplay {
  auto display = MakeLinearExistingOverlayDisplay(mapping, source, style, clip);
  if (display.mode == MaskOverlayMode::Hidden) {
    return display;
  }
  display.mode = MaskOverlayMode::Creating;
  const Vector2 n    = LinearNormal(source);
  const Vector2 perp = PerpNormalized(n);
  const float half   = std::max(source.transition_distance, kMinRadius) * 0.5f;
  const float span   = 0.35f;
  const float loci[] = {-half, 0.0f, half};
  for (float signed_distance : loci) {
    const Vector2 locus = LinearLocus(source, signed_distance);
    const Vector2 a{locus.x + perp.x * span, locus.y + perp.y * span};
    const Vector2 b{locus.x - perp.x * span, locus.y - perp.y * span};
    const auto item_a = MapNormalizedMaskPointToItem(mapping, a);
    const auto item_b = MapNormalizedMaskPointToItem(mapping, b);
    if (item_a && item_b) {
      display.creation_guides.emplace_back(*item_a, *item_b);
    }
  }
  return display;
}

auto MapReferenceRadiusToItem(const MaskEditViewMapping& mapping, Vector2 center_reference,
                              float radius_reference_px) -> std::optional<float> {
  if (!std::isfinite(radius_reference_px) || radius_reference_px <= 0.0f) {
    return std::nullopt;
  }
  const auto center = MaskEditGeometry::MapReferenceToItem(mapping, center_reference);
  const auto edge   = MaskEditGeometry::MapReferenceToItem(
      mapping, Vector2{center_reference.x + radius_reference_px, center_reference.y});
  if (!center || !edge) {
    return std::nullopt;
  }
  return ItemDistance(*center, *edge);
}

}  // namespace alcedo
