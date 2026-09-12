//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/mask/brush_rasterizer.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>

#include "edit/mask/brush_raster_encoding.hpp"
#include "edit/mask/brush_source_geometry.hpp"
#include "edit/mask/brush_stroke.hpp"

namespace alcedo {
namespace {

[[noreturn]] void FailRaster(std::string_view message) {
  throw std::runtime_error(std::string{message});
}

void RequireAlgorithm(const BrushMaskSource& source) {
  if (source.raster_algorithm_version != kBrushRasterAlgorithmVersion) {
    FailRaster("unsupported brush raster_algorithm_version");
  }
  if (source.source_format_version != kBrushSourceFormatVersion) {
    FailRaster("unsupported brush source_format_version");
  }
}

}  // namespace

void BrushRasterizer::RequireGeometry() const {
  if (raster_.Empty() || pixels_->size() !=
                             static_cast<std::size_t>(raster_.width) *
                                 static_cast<std::size_t>(raster_.height)) {
    FailRaster("brush rasterizer geometry is unset");
  }
}

void BrushRasterizer::SetGeometry(Extent2D raster, Extent2D full_reference) {
  if (raster.Empty() || full_reference.Empty()) {
    FailRaster("brush rasterizer requires positive raster and full-reference extents");
  }
  raster_          = raster;
  full_reference_  = full_reference;
  // Fresh storage: SharedPixels handles handed out for the previous geometry
  // must keep the old bytes immutable.
  pixels_ = std::make_shared<std::vector<std::uint8_t>>(
      static_cast<std::size_t>(raster.width) * raster.height, 0);
}

void BrushRasterizer::ClearToZero() {
  RequireGeometry();
  std::fill(pixels_->begin(), pixels_->end(), 0);
}

void BrushRasterizer::StampDab(const BrushCanonicalSample& sample, Vector2 translation,
                               BrushStrokeMode mode, RectI clip) {
  const auto support =
      IntersectTexelRect(BrushDabOutputTexelSupport(sample, translation, raster_, full_reference_),
                         clip);
  if (RectIEmpty(support)) {
    return;
  }
  const float world_x = sample.local_x + translation.x;
  const float world_y = sample.local_y + translation.y;
  for (std::int32_t y = support.y; y < support.Y1(); ++y) {
    for (std::int32_t x = support.x; x < support.X1(); ++x) {
      const auto center = CanonicalBrushTexelReferenceCenter(static_cast<std::uint32_t>(x),
                                                             static_cast<std::uint32_t>(y), raster_,
                                                             full_reference_);
      const float distance = std::hypot(center.x - world_x, center.y - world_y);
      const auto  dab =
          QuantizeMaskCoverageToR8(BrushDabCoverage(distance, sample.radius, sample.strength,
                                                    sample.hardness));
      const auto index = PackedR8Index(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y),
                                       raster_);
      (*pixels_)[index] = mode == BrushStrokeMode::Erase ? EraseBrushR8((*pixels_)[index], dab)
                                                          : PaintBrushR8((*pixels_)[index], dab);
    }
  }
}

void BrushRasterizer::StampOrderedSamples(std::span<const BrushCanonicalSample> samples,
                                          Vector2 translation, BrushStrokeMode mode, RectI clip) {
  RequireGeometry();
  const auto region = ClipTexelRect(clip, raster_);
  if (RectIEmpty(region)) {
    return;
  }
  for (const auto& sample : samples) {
    StampDab(sample, translation, mode, region);
  }
}

void BrushRasterizer::ApplyCoverageUpdate(const BrushMaskSource& source,
                                          const BrushSpatialIndex& index,
                                          const BrushCoverageUpdate& update) {
  RequireGeometry();
  RequireAlgorithm(source);
  if (update.kind == BrushCoverageUpdateKind::Unchanged) {
    return;
  }
  if (update.kind == BrushCoverageUpdateKind::ReplayDirty) {
    ReplayRegion(source, index, update.dirty);
    return;
  }
  if (update.stroke_index >= source.strokes.size() || update.stroke_end > source.strokes.size() ||
      update.stroke_index >= update.stroke_end) {
    FailRaster("coverage stamp stroke range is outside the source");
  }
  for (auto stroke_index = update.stroke_index; stroke_index < update.stroke_end; ++stroke_index) {
    const auto& stroke  = source.strokes[stroke_index];
    const auto  samples = BrushStrokeSamples(stroke);
    const auto  begin   = stroke_index == update.stroke_index ? update.sample_begin : 0;
    if (begin > samples.size()) {
      FailRaster("coverage stamp sample begin is outside the stroke");
    }
    StampOrderedSamples(samples.subspan(begin), source.placement_translation, stroke.mode,
                        update.dirty);
  }
}

void BrushRasterizer::ReplayRegion(const BrushMaskSource& source, const BrushSpatialIndex& index,
                                   RectI dirty) {
  RequireGeometry();
  RequireAlgorithm(source);
  if (index.Raster() != raster_ || index.FullReference() != full_reference_) {
    FailRaster("spatial index geometry does not match the rasterizer");
  }
  const auto region = ClipTexelRect(dirty, raster_);
  if (RectIEmpty(region)) {
    return;
  }
  for (std::int32_t y = region.y; y < region.Y1(); ++y) {
    for (std::int32_t x = region.x; x < region.X1(); ++x) {
      const auto px = static_cast<std::uint32_t>(x);
      const auto py = static_cast<std::uint32_t>(y);
      (*pixels_)[PackedR8Index(px, py, raster_)] = 0;
    }
  }
  const auto spans = index.QueryOutput(region, source.placement_translation);
  for (const auto& span : spans) {
    if (span.stroke_index >= source.strokes.size()) {
      FailRaster("spatial index stroke is outside the source");
    }
    const auto& stroke = source.strokes[span.stroke_index];
    if (stroke.id != span.stroke_id) {
      FailRaster("spatial index StrokeId does not match the source");
    }
    const auto samples = BrushStrokeSamples(stroke);
    if (span.sample_end > samples.size() || span.sample_begin >= span.sample_end) {
      FailRaster("spatial index sample span is outside the stroke");
    }
    for (auto sample_index = span.sample_begin; sample_index < span.sample_end; ++sample_index) {
      StampDab(samples[sample_index], source.placement_translation, stroke.mode, region);
    }
  }
}

void BrushRasterizer::RasterizeFull(const BrushMaskSource& source) {
  RequireGeometry();
  BrushSpatialIndex index;
  index.Rebuild(source, raster_, full_reference_);
  ClearToZero();
  ReplayRegion(source, index, FullTexelRect(raster_));
}

}  // namespace alcedo
