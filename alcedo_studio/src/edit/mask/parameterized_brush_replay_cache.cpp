//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/mask/parameterized_brush_replay_cache.hpp"

#include <algorithm>
#include <utility>

#include "edit/mask/brush_raster_encoding.hpp"
#include "edit/mask/brush_source_geometry.hpp"
#include "edit/mask/brush_stroke.hpp"

namespace alcedo {
namespace {

/// Output-texels that differ between the previously replayed source and @p next.
/// Stroke identity compares id, mode, and the shared sample-body pointer; a
/// placement move dirties the same stroke under both translations.
[[nodiscard]] auto BrushReplayDirtyTexels(const BrushMaskSource& previous,
                                          const BrushMaskSource& next, Extent2D raster,
                                          Extent2D full_reference) -> RectI {
  RectI      dirty{};
  const auto common = std::min(previous.strokes.size(), next.strokes.size());
  for (std::size_t i = 0; i < common; ++i) {
    const auto& before = previous.strokes[i];
    const auto& after  = next.strokes[i];
    const bool  same_stroke =
        before.id == after.id && before.mode == after.mode &&
        BrushStrokesShareSampleBody(before, after);
    if (same_stroke && previous.placement_translation == next.placement_translation) {
      continue;
    }
    dirty = UnionTexelRect(dirty, BrushStrokeOutputTexelSupport(before, previous.placement_translation,
                                                              raster, full_reference));
    dirty = UnionTexelRect(dirty, BrushStrokeOutputTexelSupport(after, next.placement_translation,
                                                                raster, full_reference));
  }
  for (std::size_t i = common; i < previous.strokes.size(); ++i) {
    dirty = UnionTexelRect(dirty, BrushStrokeOutputTexelSupport(previous.strokes[i],
                                                                previous.placement_translation,
                                                                raster, full_reference));
  }
  for (std::size_t i = common; i < next.strokes.size(); ++i) {
    dirty = UnionTexelRect(dirty, BrushStrokeOutputTexelSupport(next.strokes[i],
                                                                next.placement_translation, raster,
                                                                full_reference));
  }
  return ClipTexelRect(dirty, raster);
}

}  // namespace

auto ParameterizedBrushReplayCache::Replay(const NodeId& owner_node_id, const MaskId& mask_id,
                                           const BrushMaskSource& source, Extent2D full_reference,
                                           std::uint64_t content_revision,
                                           std::uint64_t session_generation)
    -> ActiveRasterMaskInput {
  const auto raster = CanonicalBrushRasterExtent(full_reference);
  auto&      entry  = entries_[{owner_node_id, mask_id}];
  entry.lru_tick    = ++lru_clock_;

  RectI dirty{};
  if (!entry.valid || entry.raster != raster ||
      entry.rasterizer.FullReference() != full_reference) {
    entry.rasterizer.SetGeometry(raster, full_reference);
    entry.rasterizer.RasterizeFull(source);
    entry.index.Rebuild(source, raster, full_reference);
    entry.source = source;
    entry.raster = raster;
    entry.valid  = true;
    dirty        = FullTexelRect(raster);
  } else {
    dirty = BrushReplayDirtyTexels(entry.source, source, raster, full_reference);
    if (!RectIEmpty(dirty)) {
      entry.index.Rebuild(source, raster, full_reference);
      entry.rasterizer.ReplayRegion(source, entry.index, dirty);
      entry.source = source;
    }
  }
  EvictOverflow();

  ActiveRasterMaskInput input;
  input.owner_node_id               = owner_node_id;
  input.mask_id                     = mask_id;
  input.session_generation          = session_generation;
  input.content_revision            = content_revision == 0 ? 1 : content_revision;
  input.descriptor.extent           = raster;
  input.descriptor.reference_bounds = CanonicalBrushReferenceBounds();
  // The upload contract requires a non-empty clipped rectangle even when the
  // replay found no pixel change; one texel of identical data keeps it valid.
  input.dirty_rectangle = RectIEmpty(dirty)
                              ? ClipTexelRect(RectI{0, 0, 1, 1}, raster)
                              : dirty;
  input.pixels          = entry.rasterizer.SharedPixels();
  return input;
}

void ParameterizedBrushReplayCache::Clear() { entries_.clear(); }

void ParameterizedBrushReplayCache::EvictOverflow() {
  while (entries_.size() > kMaxRetainedEntries) {
    auto victim = entries_.end();
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
      if (victim == entries_.end() || it->second.lru_tick < victim->second.lru_tick) {
        victim = it;
      }
    }
    entries_.erase(victim);
  }
}

}  // namespace alcedo
