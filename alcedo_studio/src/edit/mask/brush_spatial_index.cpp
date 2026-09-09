//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/mask/brush_spatial_index.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include "edit/mask/brush_source_geometry.hpp"

namespace alcedo {
namespace {

[[noreturn]] void FailIndex(std::string_view message) {
  throw std::runtime_error(std::string{message});
}

}  // namespace

BrushSpatialIndex::BrushSpatialIndex(std::uint32_t tile_texels) : tile_texels_(tile_texels) {
  if (tile_texels_ == 0) {
    FailIndex("spatial index tile size must be positive");
  }
}

void BrushSpatialIndex::Clear() {
  raster_            = {};
  full_reference_    = {};
  tiles_x_           = 0;
  tiles_y_           = 0;
  spans_.clear();
  tile_span_indices_.clear();
}

void BrushSpatialIndex::Rebuild(const BrushMaskSource& source, Extent2D raster,
                                Extent2D full_reference) {
  if (raster.Empty() || full_reference.Empty()) {
    FailIndex("spatial index requires positive raster and full-reference extents");
  }
  if (source.raster_algorithm_version != kBrushRasterAlgorithmVersion) {
    FailIndex("unsupported brush raster_algorithm_version");
  }
  ValidateBrushStrokeList(source.strokes);
  Clear();
  raster_         = raster;
  full_reference_ = full_reference;
  tiles_x_        = (raster.width + tile_texels_ - 1) / tile_texels_;
  tiles_y_        = (raster.height + tile_texels_ - 1) / tile_texels_;
  tile_span_indices_.assign(static_cast<std::size_t>(tiles_x_) * tiles_y_, {});
  for (std::size_t stroke_index = 0; stroke_index < source.strokes.size(); ++stroke_index) {
    const auto& stroke  = source.strokes[stroke_index];
    const auto  samples = BrushStrokeSamples(stroke);
    for (std::size_t sample_index = 0; sample_index < samples.size(); ++sample_index) {
      const auto local_bounds =
          BrushDabLocalTexelSupport(samples[sample_index], raster, full_reference);
      if (RectIEmpty(local_bounds)) {
        continue;
      }
      const auto span_index = spans_.size();
      spans_.push_back(BrushIndexedSpan{stroke.id, stroke_index, sample_index, sample_index + 1,
                                        local_bounds});
      const auto tile_x0 = static_cast<std::uint32_t>(local_bounds.x) / tile_texels_;
      const auto tile_y0 = static_cast<std::uint32_t>(local_bounds.y) / tile_texels_;
      const auto tile_x1 =
          (static_cast<std::uint32_t>(local_bounds.X1() - 1) / tile_texels_) + 1;
      const auto tile_y1 =
          (static_cast<std::uint32_t>(local_bounds.Y1() - 1) / tile_texels_) + 1;
      for (auto ty = tile_y0; ty < tile_y1 && ty < tiles_y_; ++ty) {
        for (auto tx = tile_x0; tx < tile_x1 && tx < tiles_x_; ++tx) {
          tile_span_indices_[TileIndex(tx, ty)].push_back(span_index);
        }
      }
    }
  }
}

auto BrushSpatialIndex::TileIndex(std::uint32_t tile_x, std::uint32_t tile_y) const -> std::size_t {
  return static_cast<std::size_t>(tile_y) * tiles_x_ + tile_x;
}

auto BrushSpatialIndex::UniqueSpansIn(RectI local_texels) const -> std::vector<BrushIndexedSpan> {
  const auto query = ClipTexelRect(local_texels, raster_);
  if (RectIEmpty(query) || tiles_x_ == 0 || tiles_y_ == 0) {
    return {};
  }
  const auto tile_x0 = static_cast<std::uint32_t>(query.x) / tile_texels_;
  const auto tile_y0 = static_cast<std::uint32_t>(query.y) / tile_texels_;
  const auto tile_x1 = (static_cast<std::uint32_t>(query.X1() - 1) / tile_texels_) + 1;
  const auto tile_y1 = (static_cast<std::uint32_t>(query.Y1() - 1) / tile_texels_) + 1;
  std::vector<std::size_t> ids;
  for (auto ty = tile_y0; ty < tile_y1 && ty < tiles_y_; ++ty) {
    for (auto tx = tile_x0; tx < tile_x1 && tx < tiles_x_; ++tx) {
      const auto& tile = tile_span_indices_[TileIndex(tx, ty)];
      ids.insert(ids.end(), tile.begin(), tile.end());
    }
  }
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  std::vector<BrushIndexedSpan> result;
  result.reserve(ids.size());
  for (const auto id : ids) {
    const auto& span = spans_[id];
    if (!RectIEmpty(IntersectTexelRect(span.local_bounds, query))) {
      result.push_back(span);
    }
  }
  std::sort(result.begin(), result.end(), [](const BrushIndexedSpan& a, const BrushIndexedSpan& b) {
    if (a.stroke_index != b.stroke_index) {
      return a.stroke_index < b.stroke_index;
    }
    return a.sample_begin < b.sample_begin;
  });
  return result;
}

auto BrushSpatialIndex::QueryLocal(RectI local_texels) const -> std::vector<BrushIndexedSpan> {
  if (raster_.Empty()) {
    return {};
  }
  return UniqueSpansIn(local_texels);
}

auto BrushSpatialIndex::QueryOutput(RectI output_texels, Vector2 translation) const
    -> std::vector<BrushIndexedSpan> {
  if (raster_.Empty()) {
    FailIndex("spatial index has not been rebuilt");
  }
  try {
    return UniqueSpansIn(BrushOutputTexelsToLocal(output_texels, translation, raster_,
                                                  full_reference_));
  } catch (const std::invalid_argument& ex) {
    FailIndex(ex.what());
  }
}

}  // namespace alcedo
