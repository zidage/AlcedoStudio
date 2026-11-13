//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/edit_viewer/ai_mask_prompt_geometry.hpp"

#include <QLineF>
#include <algorithm>
#include <array>
#include <cmath>

namespace alcedo {
namespace {

[[nodiscard]] auto InsideUnitSquare(Vector2 point) -> bool {
  return point.x >= 0.0f && point.x <= 1.0f && point.y >= 0.0f && point.y <= 1.0f;
}

}  // namespace

auto MapItemToAiMaskPromptPoint(const MaskEditViewMapping& mapping, const QPointF& item)
    -> std::optional<Vector2> {
  const auto sample = MaskEditGeometry::MapItemToReference(mapping, item, /*allow_outside=*/false);
  if (!sample.has_value() || !InsideUnitSquare(sample->normalized)) {
    return std::nullopt;
  }
  return sample->normalized;
}

auto MapItemRectToAiMaskPromptBox(const MaskEditViewMapping& mapping, const QPointF& corner_a,
                                  const QPointF& corner_b) -> std::optional<NormalizedRect> {
  const std::array<QPointF, 4> corners = {corner_a, QPointF(corner_b.x(), corner_a.y()), corner_b,
                                          QPointF(corner_a.x(), corner_b.y())};
  float                        min_x   = 0.0f;
  float                        min_y   = 0.0f;
  float                        max_x   = 0.0f;
  float                        max_y   = 0.0f;
  for (std::size_t index = 0; index < corners.size(); ++index) {
    const auto sample =
        MaskEditGeometry::MapItemToReference(mapping, corners[index], /*allow_outside=*/true);
    if (!sample.has_value()) {
      return std::nullopt;
    }
    const auto point = sample->normalized;
    if (index == 0) {
      min_x = max_x = point.x;
      min_y = max_y = point.y;
      continue;
    }
    min_x = std::min(min_x, point.x);
    min_y = std::min(min_y, point.y);
    max_x = std::max(max_x, point.x);
    max_y = std::max(max_y, point.y);
  }
  min_x = std::clamp(min_x, 0.0f, 1.0f);
  min_y = std::clamp(min_y, 0.0f, 1.0f);
  max_x = std::clamp(max_x, 0.0f, 1.0f);
  max_y = std::clamp(max_y, 0.0f, 1.0f);
  if (max_x <= min_x || max_y <= min_y) {
    return std::nullopt;
  }
  return NormalizedRect{min_x, min_y, max_x - min_x, max_y - min_y};
}

auto MapItemStrokeToAiMaskPromptPoints(const MaskEditViewMapping&  mapping,
                                       const std::vector<QPointF>& item_path,
                                       std::size_t maximum_points) -> std::vector<Vector2> {
  std::vector<Vector2> points;
  if (item_path.empty() || maximum_points == 0) {
    return points;
  }
  // Arc length at each pointer sample.
  std::vector<qreal> lengths(item_path.size(), 0.0);
  for (std::size_t index = 1; index < item_path.size(); ++index) {
    lengths[index] = lengths[index - 1] + QLineF(item_path[index - 1], item_path[index]).length();
  }
  const qreal total = lengths.back();
  const auto  count = std::clamp<std::size_t>(
      static_cast<std::size_t>(std::lround(total / kAiMaskStrokePromptSpacingItemPx)) + 1, 1,
      maximum_points);
  std::size_t segment = 1;
  for (std::size_t sample = 0; sample < count; ++sample) {
    QPointF item = item_path.front();
    if (count > 1) {
      const qreal target = total * static_cast<qreal>(sample) / static_cast<qreal>(count - 1);
      while (segment + 1 < item_path.size() && lengths[segment] < target) {
        ++segment;
      }
      const auto& a      = item_path[std::min(segment, item_path.size() - 1) - 1];
      const auto& b      = item_path[std::min(segment, item_path.size() - 1)];
      const qreal start  = lengths[std::min(segment, item_path.size() - 1) - 1];
      const qreal length = QLineF(a, b).length();
      const qreal t      = length > 0.0 ? std::clamp((target - start) / length, 0.0, 1.0) : 0.0;
      item               = a + (b - a) * t;
    }
    if (const auto point = MapItemToAiMaskPromptPoint(mapping, item); point.has_value()) {
      points.push_back(*point);
    }
  }
  return points;
}

auto AiMaskAnalysisToItemTransform(const MaskEditViewMapping& mapping)
    -> std::optional<QTransform> {
  if (!MaskEditGeometry::IsValid(mapping)) {
    return std::nullopt;
  }
  const auto& full   = mapping.geometry.full_reference_extent;
  const auto  origin = MaskEditGeometry::MapReferenceToItem(mapping, Vector2{0.0f, 0.0f});
  const auto  x_axis =
      MaskEditGeometry::MapReferenceToItem(mapping, Vector2{static_cast<float>(full.width), 0.0f});
  const auto y_axis =
      MaskEditGeometry::MapReferenceToItem(mapping, Vector2{0.0f, static_cast<float>(full.height)});
  if (!origin.has_value() || !x_axis.has_value() || !y_axis.has_value()) {
    return std::nullopt;
  }
  // QTransform maps (u, v) to (m11 u + m21 v + dx, m12 u + m22 v + dy).
  return QTransform(x_axis->x() - origin->x(), x_axis->y() - origin->y(), y_axis->x() - origin->x(),
                    y_axis->y() - origin->y(), origin->x(), origin->y());
}

}  // namespace alcedo
