//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/justified_row_layout.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

namespace alcedo::ui {

auto JustifiedRowLayout::ClampAspect(double aspect_ratio) -> double {
  if (!(aspect_ratio > 0.0) || !std::isfinite(aspect_ratio)) {
    return kUnknownAspectRatio;
  }
  return std::clamp(aspect_ratio, kMinAspectRatio, kMaxAspectRatio);
}

auto JustifiedRowLayout::UsableGeometry() const -> bool {
  return geometry_.view_width > 0.0 && geometry_.row_height > 0.0 && geometry_.spacing >= 0.0 &&
         geometry_.caption_height >= 0.0;
}

auto JustifiedRowLayout::SetGeometry(const JustifiedLayoutGeometry& geometry) -> bool {
  if (geometry == geometry_) {
    first_changed_row_ = -1;
    return false;
  }
  geometry_ = geometry;
  LayoutFromRow(0);
  return true;
}

void JustifiedRowLayout::Reset(std::vector<double> aspect_ratios) {
  aspects_    = std::move(aspect_ratios);
  aspect_sum_ = 0.0;
  for (auto& aspect : aspects_) {
    aspect = ClampAspect(aspect);
    aspect_sum_ += aspect;
  }
  rows_.clear();
  LayoutFromRow(0);
}

void JustifiedRowLayout::Append(const std::vector<double>& aspect_ratios) {
  Insert(ItemCount(), aspect_ratios);
}

void JustifiedRowLayout::Insert(int index, const std::vector<double>& aspect_ratios) {
  index = std::clamp(index, 0, ItemCount());
  std::vector<double> clamped;
  clamped.reserve(aspect_ratios.size());
  for (const auto aspect : aspect_ratios) {
    clamped.push_back(ClampAspect(aspect));
    aspect_sum_ += clamped.back();
  }
  aspects_.insert(aspects_.begin() + index, clamped.begin(), clamped.end());
  LayoutFromIndex(index);
}

void JustifiedRowLayout::Remove(int first, int count) {
  first = std::clamp(first, 0, ItemCount());
  count = std::clamp(count, 0, ItemCount() - first);
  if (count == 0) {
    first_changed_row_ = -1;
    return;
  }
  for (int i = first; i < first + count; ++i) {
    aspect_sum_ -= aspects_[static_cast<size_t>(i)];
  }
  aspects_.erase(aspects_.begin() + first, aspects_.begin() + first + count);
  if (aspects_.empty()) {
    aspect_sum_ = 0.0;
  }
  LayoutFromIndex(first);
}

auto JustifiedRowLayout::SetAspectRatio(int index, double aspect_ratio) -> bool {
  if (index < 0 || index >= ItemCount()) {
    first_changed_row_ = -1;
    return false;
  }
  const double clamped = ClampAspect(aspect_ratio);
  auto&        stored  = aspects_[static_cast<size_t>(index)];
  if (clamped == stored) {
    first_changed_row_ = -1;
    return false;
  }
  aspect_sum_ += clamped - stored;
  stored = clamped;
  LayoutFromIndex(index);
  return true;
}

void JustifiedRowLayout::LayoutFromIndex(int index) {
  if (rows_.empty()) {
    LayoutFromRow(0);
    return;
  }
  const int row = RowOfIndex(index);
  LayoutFromRow(row >= 0 ? row : static_cast<int>(rows_.size()) - 1);
}

void JustifiedRowLayout::LayoutFromRow(int row_index) {
  row_index             = std::clamp(row_index, 0, static_cast<int>(rows_.size()));
  const int count_items = ItemCount();
  tile_x_.resize(aspects_.size());
  tile_width_.resize(aspects_.size());
  if (!UsableGeometry()) {
    rows_.clear();
    first_changed_row_ = 0;
    return;
  }

  int    index = 0;
  double top   = 0.0;
  if (row_index > 0) {
    const auto& previous = rows_[static_cast<size_t>(row_index) - 1];
    index                = previous.first + previous.count;
    top                  = previous.top + TileHeight(previous.photo_height) + geometry_.spacing;
  }
  rows_.resize(static_cast<size_t>(row_index));
  first_changed_row_   = row_index;

  const double width   = geometry_.view_width;
  const double target  = geometry_.row_height;
  const double spacing = geometry_.spacing;
  while (index < count_items) {
    double sum      = 0.0;
    int    count    = 0;
    double height   = target;
    bool   complete = false;
    for (int j = index; j < count_items; ++j) {
      sum += aspects_[static_cast<size_t>(j)];
      ++count;
      const double fitted = (width - (count - 1) * spacing) / sum;
      if (fitted > target) {
        continue;
      }
      // The row is full. End it before photo j when that height is nearer to the target.
      height = fitted;
      if (count > 1) {
        const double previous_sum    = sum - aspects_[static_cast<size_t>(j)];
        const double previous_height = (width - (count - 2) * spacing) / previous_sum;
        if (previous_height - target < target - fitted &&
            previous_height <= target * kMaxRowHeightRatio) {
          --count;
          sum    = previous_sum;
          height = previous_height;
        }
      }
      complete = true;
      break;
    }

    const double available = width - (count - 1) * spacing;
    if (complete) {
      height = std::max(height, target * kMinRowHeightRatio);
    }
    double x = 0.0;
    for (int k = 0; k < count; ++k) {
      const auto   item   = static_cast<size_t>(index + k);
      // A complete row divides the view width by aspect ratio; the last row keeps the target.
      const double tile_w = complete ? available * aspects_[item] / sum : aspects_[item] * target;
      tile_x_[item]       = x;
      tile_width_[item]   = tile_w;
      x += tile_w + spacing;
    }
    rows_.push_back(Row{index, count, top, height, complete});
    top += TileHeight(height) + spacing;
    index += count;
  }
}

auto JustifiedRowLayout::TileRect(int index) const -> std::optional<JustifiedTileRect> {
  const int row = RowOfIndex(index);
  if (row < 0) {
    return std::nullopt;
  }
  const auto& r    = rows_[static_cast<size_t>(row)];
  const auto  item = static_cast<size_t>(index);
  return JustifiedTileRect{tile_x_[item], r.top, tile_width_[item], TileHeight(r.photo_height)};
}

auto JustifiedRowLayout::RowOfIndex(int index) const -> int {
  if (index < 0 || rows_.empty()) {
    return -1;
  }
  const auto it = std::upper_bound(rows_.begin(), rows_.end(), index,
                                   [](int value, const Row& row) { return value < row.first; });
  if (it == rows_.begin()) {
    return -1;
  }
  const auto& row = *std::prev(it);
  if (index >= row.first + row.count) {
    return -1;
  }
  return static_cast<int>(std::distance(rows_.begin(), it)) - 1;
}

auto JustifiedRowLayout::RowAtY(double y) const -> int {
  if (rows_.empty()) {
    return -1;
  }
  const auto it = std::upper_bound(rows_.begin(), rows_.end(), y,
                                   [](double value, const Row& row) { return value < row.top; });
  if (it == rows_.begin()) {
    return 0;
  }
  return static_cast<int>(std::distance(rows_.begin(), it)) - 1;
}

auto JustifiedRowLayout::IndexAt(double x, double y) const -> int {
  const int row_index = RowAtY(y);
  if (row_index < 0) {
    return -1;
  }
  const auto& row = rows_[static_cast<size_t>(row_index)];
  if (y < row.top || y >= row.top + TileHeight(row.photo_height)) {
    return -1;
  }
  for (int k = 0; k < row.count; ++k) {
    const auto item = static_cast<size_t>(row.first + k);
    if (x >= tile_x_[item] && x < tile_x_[item] + tile_width_[item]) {
      return row.first + k;
    }
  }
  return -1;
}

auto JustifiedRowLayout::NearestIndex(double x, double y) const -> int {
  const int row_index = RowAtY(y);
  if (row_index < 0) {
    return -1;
  }
  const auto& row          = rows_[static_cast<size_t>(row_index)];
  int         nearest      = row.first;
  double      nearest_dist = -1.0;
  for (int k = 0; k < row.count; ++k) {
    const auto   item  = static_cast<size_t>(row.first + k);
    const double left  = tile_x_[item];
    const double right = left + tile_width_[item];
    const double dist  = x < left ? left - x : (x >= right ? x - right : 0.0);
    if (nearest_dist < 0.0 || dist < nearest_dist) {
      nearest      = row.first + k;
      nearest_dist = dist;
    }
  }
  return nearest;
}

auto JustifiedRowLayout::IndicesInRect(double x0, double y0, double x1, double y1) const
    -> std::vector<int> {
  std::vector<int> out;
  if (rows_.empty() || x1 < x0 || y1 < y0) {
    return out;
  }
  for (int r = RowAtY(y0); r >= 0 && r < static_cast<int>(rows_.size()); ++r) {
    const auto& row = rows_[static_cast<size_t>(r)];
    if (row.top > y1) {
      break;
    }
    if (row.top + TileHeight(row.photo_height) < y0) {
      continue;
    }
    for (int k = 0; k < row.count; ++k) {
      const auto item = static_cast<size_t>(row.first + k);
      if (tile_x_[item] <= x1 && tile_x_[item] + tile_width_[item] >= x0) {
        out.push_back(row.first + k);
      }
    }
  }
  return out;
}

auto JustifiedRowLayout::IndexRangeForY(double y0, double y1) const -> IndexRange {
  if (rows_.empty() || y1 < y0) {
    return {};
  }
  int first = RowAtY(y0);
  if (rows_[static_cast<size_t>(first)].top +
          TileHeight(rows_[static_cast<size_t>(first)].photo_height) <
      y0) {
    ++first;
  }
  const int last = RowAtY(y1);
  if (first >= static_cast<int>(rows_.size()) || first > last ||
      rows_[static_cast<size_t>(last)].top > y1) {
    return {};
  }
  const auto& end_row = rows_[static_cast<size_t>(last)];
  return IndexRange{rows_[static_cast<size_t>(first)].first, end_row.first + end_row.count - 1};
}

auto JustifiedRowLayout::LoadedHeight() const -> double {
  if (rows_.empty()) {
    return 0.0;
  }
  return rows_.back().top + TileHeight(rows_.back().photo_height);
}

auto JustifiedRowLayout::EstimatedPhotosPerRow() const -> int {
  const double mean =
      aspects_.empty() ? kUnknownAspectRatio : aspect_sum_ / static_cast<double>(aspects_.size());
  const double per_row = (geometry_.view_width + geometry_.spacing) /
                         (mean * geometry_.row_height + geometry_.spacing);
  return std::max(1, static_cast<int>(std::floor(per_row)));
}

auto JustifiedRowLayout::EstimatedRowPitch() const -> double {
  return TileHeight(geometry_.row_height) + geometry_.spacing;
}

auto JustifiedRowLayout::EstimatedHeight(int total_count) const -> double {
  const int remaining = total_count - ItemCount();
  if (remaining <= 0 || !UsableGeometry()) {
    return LoadedHeight();
  }
  const int    per_row = EstimatedPhotosPerRow();
  const int    rows    = (remaining + per_row - 1) / per_row;
  const double pitch   = EstimatedRowPitch();
  if (rows_.empty()) {
    return rows * pitch - geometry_.spacing;
  }
  return LoadedHeight() + rows * pitch;
}

auto JustifiedRowLayout::EstimatedRowTop(int index) const -> double {
  if (index < 0) {
    return 0.0;
  }
  const int row = RowOfIndex(index);
  if (row >= 0) {
    return rows_[static_cast<size_t>(row)].top;
  }
  if (!UsableGeometry()) {
    return 0.0;
  }
  const double base = rows_.empty() ? 0.0 : LoadedHeight() + geometry_.spacing;
  const int    step = (index - ItemCount()) / EstimatedPhotosPerRow();
  return base + std::max(0, step) * EstimatedRowPitch();
}

auto JustifiedRowLayout::EstimatedIndexAtY(double y) const -> int {
  if (!rows_.empty() && y < LoadedHeight()) {
    return rows_[static_cast<size_t>(RowAtY(y))].first;
  }
  if (!UsableGeometry()) {
    return ItemCount();
  }
  const double base = rows_.empty() ? 0.0 : LoadedHeight() + geometry_.spacing;
  const int    step = std::max(0, static_cast<int>(std::floor((y - base) / EstimatedRowPitch())));
  return ItemCount() + step * EstimatedPhotosPerRow();
}

}  // namespace alcedo::ui
