//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/edit_viewer/mask_overlay_layout.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

#include "ui/edit_viewer/crop_geometry.hpp"
#include "ui/edit_viewer/viewport_mapper.hpp"

namespace alcedo {
namespace {

constexpr float    kPi          = 3.14159265358979323846f;
constexpr float    kMinRadius   = 1.0e-5f;
constexpr float    kHandleMerge = 2.0f;
constexpr float    kRhoCoincide = 1.0e-3f;
constexpr float    kGuideSpan   = 8.0f;

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
                  const QPointF& origin, float merge_px, MaskOverlayHandleShape shape) {
  if (id != MaskOverlayHandleId::RadialCenter && id != MaskOverlayHandleId::LinearOrigin &&
      id != MaskOverlayHandleId::BrushMove && ItemDistance(item, origin) < merge_px) {
    return;
  }
  for (const auto& existing : display.handles) {
    if (ItemDistance(existing.item, item) < merge_px) {
      return;
    }
  }
  display.handles.push_back(MaskOverlayHandle{id, item, shape});
}

[[nodiscard]] auto DistanceToSegment(QPointF point, QPointF a, QPointF b) -> float {
  const double dx   = b.x() - a.x();
  const double dy   = b.y() - a.y();
  const double len2 = dx * dx + dy * dy;
  if (len2 < static_cast<double>(kMinRadius) * static_cast<double>(kMinRadius)) {
    return ItemDistance(point, a);
  }
  double t = ((point.x() - a.x()) * dx + (point.y() - a.y()) * dy) / len2;
  t        = std::clamp(t, 0.0, 1.0);
  return ItemDistance(point, QPointF(a.x() + t * dx, a.y() + t * dy));
}

[[nodiscard]] auto ClipSegmentToRect(QPointF a, QPointF b, const QRectF& clip)
    -> std::optional<std::pair<QPointF, QPointF>> {
  if (!clip.isValid() || clip.isEmpty()) {
    if (!std::isfinite(a.x()) || !std::isfinite(a.y()) || !std::isfinite(b.x()) ||
        !std::isfinite(b.y())) {
      return std::nullopt;
    }
    return std::make_pair(a, b);
  }
  const double dx   = b.x() - a.x();
  const double dy   = b.y() - a.y();
  double       t0   = 0.0;
  double       t1   = 1.0;
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
  const float   len   = ItemDistance(along, origin);
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
  const auto center =
      MapNormalizedMaskPointToItem(mapping, Vector2{source.center_x, source.center_y});
  if (!center) {
    return;
  }
  display.handles.push_back(
      MaskOverlayHandle{MaskOverlayHandleId::RadialCenter, *center, MaskOverlayHandleShape::Disc});

  const auto major = MapRho(mapping, source, 1.0f, 0.0f);
  const auto minor = MapRho(mapping, source, 1.0f, kPi * 0.5f);
  if (major) {
    AppendHandle(display, MaskOverlayHandleId::RadialMajor, *major, *center, kHandleMerge,
                 MaskOverlayHandleShape::Disc);
    AppendConnector(display, *center, *major);
    if (const auto rotate =
            OffsetAlongItem(*center, *major, style.rotate_handle_offset_logical_px)) {
      if (clip.isEmpty() || clip.contains(*rotate)) {
        AppendHandle(display, MaskOverlayHandleId::RadialRotate, *rotate, *center, kHandleMerge,
                     MaskOverlayHandleShape::Disc);
        AppendConnector(display, *major, *rotate);
      }
    }
  }
  if (minor) {
    AppendHandle(display, MaskOverlayHandleId::RadialMinor, *minor, *center, kHandleMerge,
                 MaskOverlayHandleShape::Disc);
    AppendConnector(display, *center, *minor);
  }

  const float inner = InnerRho(source);
  const float outer = OuterRho(source);
  if (inner > kMinRadius && std::fabs(inner - 1.0f) > kRhoCoincide) {
    if (const auto inner_item = MapRho(mapping, source, inner, 0.0f)) {
      AppendHandle(display, MaskOverlayHandleId::RadialInnerFeather, *inner_item, *center,
                   kHandleMerge, MaskOverlayHandleShape::Ring);
    }
  }
  if (std::fabs(outer - 1.0f) > kRhoCoincide) {
    if (const auto outer_item = MapRho(mapping, source, outer, 0.0f)) {
      AppendHandle(display, MaskOverlayHandleId::RadialOuterFeather, *outer_item, *center,
                   kHandleMerge, MaskOverlayHandleShape::Ring);
    }
  }
}

void AppendRadialContour(MaskOverlayDisplay& display, const MaskEditViewMapping& mapping,
                         const RadialMaskSource& source, float rho, bool dashed,
                         const QRectF& clip) {
  if (!std::isfinite(rho) || rho < kMinRadius) {
    return;
  }
  auto polyline = TessellateRadialBoundaryItemPolyline(mapping, source, rho, clip);
  if (polyline.size() < 3) {
    return;
  }
  display.selected_contours.push_back(MaskOverlayContour{std::move(polyline), dashed});
}

void PopulateRadialContours(MaskOverlayDisplay& display, const MaskEditViewMapping& mapping,
                            const RadialMaskSource& source, const QRectF& clip) {
  const float        rhos[] = {1.0f, InnerRho(source), OuterRho(source)};
  std::vector<float> published;
  published.reserve(3);
  for (float rho : rhos) {
    bool duplicate = false;
    for (float existing : published) {
      if (std::fabs(existing - rho) <= kRhoCoincide) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      continue;
    }
    published.push_back(rho);
    // Feather boundaries are dashed; the base rho = 1 ellipse stays solid.
    AppendRadialContour(display, mapping, source, rho, std::fabs(rho - 1.0f) > kRhoCoincide,
                        clip);
  }
}

void AppendClippedGuide(MaskOverlayDisplay& display, const MaskEditViewMapping& mapping,
                        const LinearGradientMaskSource& source, float signed_distance,
                        MaskOverlayHandleId id, const MaskOverlayStyle& style,
                        const QRectF& photograph) {
  const Vector2 n     = LinearNormal(source);
  const Vector2 perp  = PerpNormalized(n);
  const Vector2 locus = LinearLocus(source, signed_distance);
  const Vector2 a{locus.x + perp.x * kGuideSpan, locus.y + perp.y * kGuideSpan};
  const Vector2 b{locus.x - perp.x * kGuideSpan, locus.y - perp.y * kGuideSpan};
  const auto    item_a = MapNormalizedMaskPointToItem(mapping, a);
  const auto    item_b = MapNormalizedMaskPointToItem(mapping, b);
  if (!item_a || !item_b) {
    return;
  }
  const auto clipped = ClipSegmentToRect(*item_a, *item_b, photograph);
  if (!clipped) {
    return;
  }
  display.selected_guides.push_back(MaskOverlayGuide{id, clipped->first, clipped->second});
  const QPointF grip_a =
      CropGeometry::LerpPoint(clipped->first, clipped->second, style.grip_span_t0);
  const QPointF grip_b =
      CropGeometry::LerpPoint(clipped->first, clipped->second, style.grip_span_t1);
  if (ItemDistance(grip_a, grip_b) >= kMinRadius) {
    display.edge_grips.emplace_back(grip_a, grip_b);
  }
}

void PopulateLinearHandles(MaskOverlayDisplay& display, const MaskEditViewMapping& mapping,
                           const LinearGradientMaskSource& source, const MaskOverlayStyle& style,
                           const QRectF& clip) {
  const auto origin =
      MapNormalizedMaskPointToItem(mapping, Vector2{source.origin_x, source.origin_y});
  if (!origin) {
    return;
  }
  display.handles.push_back(
      MaskOverlayHandle{MaskOverlayHandleId::LinearOrigin, *origin, MaskOverlayHandleShape::Disc});

  const float half  = std::max(source.transition_distance, kMinRadius) * 0.5f;
  const auto  start = MapLinearLocusNormalPointToItem(mapping, source, -half);
  const auto  end   = MapLinearLocusNormalPointToItem(mapping, source, half);
  if (start) {
    AppendHandle(display, MaskOverlayHandleId::LinearStartBoundary, *start, *origin, kHandleMerge,
                 MaskOverlayHandleShape::Disc);
  }
  if (end) {
    AppendHandle(display, MaskOverlayHandleId::LinearEndBoundary, *end, *origin, kHandleMerge,
                 MaskOverlayHandleShape::Disc);
    if (const auto direction = OffsetAlongItem(
            *origin, *end, ItemDistance(*origin, *end) + style.rotate_handle_offset_logical_px)) {
      if (clip.isEmpty() || clip.contains(*direction)) {
        AppendHandle(display, MaskOverlayHandleId::LinearDirection, *direction, *origin,
                     kHandleMerge, MaskOverlayHandleShape::Disc);
        AppendConnector(display, *end, *direction);
      }
    }
  }

  const QRectF photograph = PhotographItemRect(mapping);
  const QRectF guide_clip = photograph.isEmpty() ? clip : photograph;
  AppendClippedGuide(display, mapping, source, -half, MaskOverlayHandleId::LinearStartBoundary,
                     style, guide_clip);
  AppendClippedGuide(display, mapping, source, 0.0f, MaskOverlayHandleId::LinearOrigin, style,
                     guide_clip);
  AppendClippedGuide(display, mapping, source, half, MaskOverlayHandleId::LinearEndBoundary, style,
                     guide_clip);
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

auto MapLinearLocusNormalPointToItem(const MaskEditViewMapping&      mapping,
                                     const LinearGradientMaskSource& source, float signed_distance)
    -> std::optional<QPointF> {
  if (!std::isfinite(signed_distance)) {
    return std::nullopt;
  }
  const Vector2 origin{source.origin_x, source.origin_y};
  const Vector2 normal      = LinearNormal(source);
  const Vector2 tangent     = PerpNormalized(normal);
  const auto    item_origin = MapNormalizedMaskPointToItem(mapping, origin);
  const auto    item_normal =
      MapNormalizedMaskPointToItem(mapping, Vector2{origin.x + normal.x, origin.y + normal.y});
  const auto item_tangent =
      MapNormalizedMaskPointToItem(mapping, Vector2{origin.x + tangent.x, origin.y + tangent.y});
  if (!item_origin || !item_normal || !item_tangent) {
    return std::nullopt;
  }
  const QPointF mapped_normal          = *item_normal - *item_origin;
  const QPointF mapped_tangent         = *item_tangent - *item_origin;
  const double  tangent_length_squared = QPointF::dotProduct(mapped_tangent, mapped_tangent);
  if (!std::isfinite(tangent_length_squared) || tangent_length_squared <= kMinRadius) {
    return std::nullopt;
  }
  const double tangent_offset = -static_cast<double>(signed_distance) *
                                QPointF::dotProduct(mapped_normal, mapped_tangent) /
                                tangent_length_squared;
  return MapNormalizedMaskPointToItem(
      mapping,
      Vector2{
          origin.x + normal.x * signed_distance + tangent.x * static_cast<float>(tangent_offset),
          origin.y + normal.y * signed_distance + tangent.y * static_cast<float>(tangent_offset)});
}

auto MapItemPointToLinearDirectionSample(const MaskEditViewMapping&      mapping,
                                         const LinearGradientMaskSource& source, QPointF item)
    -> std::optional<Vector2> {
  if (!std::isfinite(item.x()) || !std::isfinite(item.y())) {
    return std::nullopt;
  }
  const Vector2 origin{source.origin_x, source.origin_y};
  const auto    item_origin = MapNormalizedMaskPointToItem(mapping, origin);
  const auto    item_x = MapNormalizedMaskPointToItem(mapping, Vector2{origin.x + 1.0f, origin.y});
  const auto    item_y = MapNormalizedMaskPointToItem(mapping, Vector2{origin.x, origin.y + 1.0f});
  if (!item_origin || !item_x || !item_y) {
    return std::nullopt;
  }
  const QPointF direction = item - *item_origin;
  const QPointF mapped_x  = *item_x - *item_origin;
  const QPointF mapped_y  = *item_y - *item_origin;
  const float   nx        = static_cast<float>(QPointF::dotProduct(mapped_x, direction));
  const float   ny        = static_cast<float>(QPointF::dotProduct(mapped_y, direction));
  const float   length    = std::hypot(nx, ny);
  if (!std::isfinite(length) || length <= kMinRadius) {
    return std::nullopt;
  }
  return Vector2{origin.x + nx / length, origin.y + ny / length};
}

auto RadialNormalizedPoint(const RadialMaskSource& source, float rho, float theta_radians)
    -> Vector2 {
  const float c  = std::cos(source.rotation);
  const float s  = std::sin(source.rotation);
  const float u  = rho * std::cos(theta_radians);
  const float v  = rho * std::sin(theta_radians);
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

  constexpr int        kSeed = 32;
  std::vector<float>   thetas;
  std::vector<QPointF> points;
  thetas.reserve(kSeed);
  points.reserve(kSeed);
  for (int i = 0; i < kSeed; ++i) {
    const float theta = (static_cast<float>(i) / static_cast<float>(kSeed)) * (2.0f * kPi);
    const auto  item  = map_theta(theta);
    if (!item) {
      return {};
    }
    thetas.push_back(theta);
    points.push_back(*item);
  }

  bool grew = true;
  while (grew && static_cast<int>(points.size()) < kMaskOverlayMaxEllipseVertices) {
    grew = false;
    std::vector<float>   next_thetas;
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
      const auto  mid_item  = map_theta(mid_theta);
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
  PopulateRadialContours(display, mapping, source, display.clip_rect);
  if (display.handles.empty()) {
    display.mode        = MaskOverlayMode::Hidden;
    display.source_kind = MaskOverlaySourceKind::None;
  }
  return display;
}

auto MakeLinearExistingOverlayDisplay(const MaskEditViewMapping&      mapping,
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
  display.mode           = MaskOverlayMode::Creating;
  display.source_kind    = MaskOverlaySourceKind::Brush;
  display.clip_rect      = clip;
  display.creation_path  = item_path;
  display.cursor_visible = std::isfinite(cursor_item.x()) && std::isfinite(cursor_item.y()) &&
                           cursor_radius_logical_px > 0.0f;
  display.cursor_center            = cursor_item;
  display.cursor_radius_logical_px = cursor_radius_logical_px;
  return display;
}

auto MakeRadialCreatingOverlayDisplay(const MaskEditViewMapping& mapping,
                                      const RadialMaskSource& source, const MaskOverlayStyle& style,
                                      const QRectF& clip) -> MaskOverlayDisplay {
  auto display = MakeRadialExistingOverlayDisplay(mapping, source, style, clip);
  if (display.mode == MaskOverlayMode::Hidden) {
    return display;
  }
  display.mode = MaskOverlayMode::Creating;
  return display;
}

auto MakeLinearCreatingOverlayDisplay(const MaskEditViewMapping&      mapping,
                                      const LinearGradientMaskSource& source,
                                      const MaskOverlayStyle& style, const QRectF& clip)
    -> MaskOverlayDisplay {
  auto display = MakeLinearExistingOverlayDisplay(mapping, source, style, clip);
  if (display.mode == MaskOverlayMode::Hidden) {
    return display;
  }
  display.mode = MaskOverlayMode::Creating;
  return display;
}

auto PhotographItemRect(const MaskEditViewMapping& mapping) -> QRectF {
  if (!MaskEditGeometry::IsValid(mapping)) {
    return {};
  }
  const auto top_left = ViewportMapper::ImageUvToWidgetPoint(
      QPointF(0.0, 0.0), mapping.widget, mapping.photograph, mapping.zoom, mapping.pan, false);
  const auto bottom_right = ViewportMapper::ImageUvToWidgetPoint(
      QPointF(1.0, 1.0), mapping.widget, mapping.photograph, mapping.zoom, mapping.pan, false);
  if (!top_left || !bottom_right) {
    return {};
  }
  return QRectF(*top_left, *bottom_right).normalized();
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

auto HitTestMaskOverlayHandle(const MaskOverlayDisplay& display, QPointF item,
                              float hit_radius_logical_px) -> MaskOverlayHandleId {
  if (display.mode == MaskOverlayMode::Hidden || hit_radius_logical_px <= 0.0f ||
      !std::isfinite(item.x()) || !std::isfinite(item.y())) {
    return MaskOverlayHandleId::None;
  }
  MaskOverlayHandleId hit  = MaskOverlayHandleId::None;
  float               best = hit_radius_logical_px;
  for (const auto& handle : display.handles) {
    if (handle.id == MaskOverlayHandleId::None || !std::isfinite(handle.item.x()) ||
        !std::isfinite(handle.item.y())) {
      continue;
    }
    const float distance = ItemDistance(item, handle.item);
    if (distance <= best) {
      best = distance;
      hit  = handle.id;
    }
  }
  if (hit != MaskOverlayHandleId::None) {
    return hit;
  }
  for (const auto& guide : display.selected_guides) {
    if (guide.id == MaskOverlayHandleId::None) {
      continue;
    }
    const float distance = DistanceToSegment(item, guide.a, guide.b);
    if (distance <= best) {
      best = distance;
      hit  = guide.id;
    }
  }
  return hit;
}

}  // namespace alcedo
