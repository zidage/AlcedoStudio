//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/mask/brush_coverage_update.hpp"

#include <algorithm>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

#include "edit/mask/brush_raster_encoding.hpp"
#include "edit/mask/brush_source_geometry.hpp"
#include "edit/mask/brush_stroke.hpp"

namespace alcedo {
namespace {

[[nodiscard]] auto SamplesEqual(std::span<const BrushCanonicalSample> lhs,
                                std::span<const BrushCanonicalSample> rhs) -> bool {
  return lhs.size() == rhs.size() && std::equal(lhs.begin(), lhs.end(), rhs.begin());
}

[[nodiscard]] auto SamplesArePrefix(std::span<const BrushCanonicalSample> prefix,
                                    std::span<const BrushCanonicalSample> full) -> bool {
  return prefix.size() <= full.size() &&
         std::equal(prefix.begin(), prefix.end(), full.begin());
}

[[nodiscard]] auto StrokeIdentityMatches(const BrushStroke& lhs, const BrushStroke& rhs) -> bool {
  return lhs.id == rhs.id && lhs.mode == rhs.mode;
}

[[nodiscard]] auto UnionNewSampleSupports(const BrushStroke& stroke, Vector2 translation,
                                          std::size_t sample_begin, std::size_t sample_end,
                                          Extent2D raster, Extent2D full_reference) -> RectI {
  RectI       dirty{};
  const auto  samples = BrushStrokeSamples(stroke);
  const auto  end     = std::min(sample_end, samples.size());
  for (auto i = sample_begin; i < end; ++i) {
    dirty = UnionTexelRect(dirty, BrushDabOutputTexelSupport(samples[i], translation, raster,
                                                             full_reference));
  }
  return ClipTexelRect(dirty, raster);
}

[[nodiscard]] auto ChangedStrokeDirty(const BrushMaskSource& previous, const BrushMaskSource& next,
                                      Extent2D raster, Extent2D full_reference) -> RectI {
  RectI      dirty{};
  const auto common = std::min(previous.strokes.size(), next.strokes.size());
  for (std::size_t i = 0; i < common; ++i) {
    const auto& before = previous.strokes[i];
    const auto& after  = next.strokes[i];
    if (StrokeIdentityMatches(before, after) &&
        SamplesEqual(BrushStrokeSamples(before), BrushStrokeSamples(after)) &&
        previous.placement_translation == next.placement_translation) {
      continue;
    }
    dirty = UnionTexelRect(
        dirty, BrushStrokeOutputTexelSupport(before, previous.placement_translation, raster,
                                             full_reference));
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
  dirty = ClipTexelRect(dirty, raster);
  if (RectIEmpty(dirty)) {
    return ClipTexelRect(RectI{0, 0, 1, 1}, raster);
  }
  return dirty;
}

[[nodiscard]] auto OneTexelDirty(Extent2D raster) -> RectI {
  return ClipTexelRect(RectI{0, 0, 1, 1}, raster);
}

}  // namespace

auto DetectBrushCoverageUpdate(const BrushMaskSource* previous, Extent2D prev_raster,
                               Extent2D prev_full_reference, const BrushMaskSource& next,
                               Extent2D raster, Extent2D full_reference) -> BrushCoverageUpdate {
  if (raster.Empty() || full_reference.Empty()) {
    throw std::invalid_argument("DetectBrushCoverageUpdate: extents must be positive");
  }
  if (next.raster_algorithm_version != kBrushRasterAlgorithmVersion ||
      next.source_format_version != kBrushSourceFormatVersion) {
    throw std::runtime_error("DetectBrushCoverageUpdate: unsupported brush algorithm version");
  }

  BrushCoverageUpdate update;
  if (previous == nullptr || prev_raster != raster || prev_full_reference != full_reference) {
    update.kind  = BrushCoverageUpdateKind::ReplayDirty;
    update.dirty = FullTexelRect(raster);
    return update;
  }
  if (previous->raster_algorithm_version != next.raster_algorithm_version ||
      previous->source_format_version != next.source_format_version) {
    update.kind  = BrushCoverageUpdateKind::ReplayDirty;
    update.dirty = FullTexelRect(raster);
    return update;
  }
  if (previous->placement_translation != next.placement_translation) {
    update.kind  = BrushCoverageUpdateKind::ReplayDirty;
    update.dirty = ChangedStrokeDirty(*previous, next, raster, full_reference);
    return update;
  }

  const auto n_prev = previous->strokes.size();
  const auto n_next = next.strokes.size();
  std::size_t equal = 0;
  while (equal < n_prev && equal < n_next &&
         StrokeIdentityMatches(previous->strokes[equal], next.strokes[equal]) &&
         SamplesEqual(BrushStrokeSamples(previous->strokes[equal]),
                      BrushStrokeSamples(next.strokes[equal]))) {
    ++equal;
  }

  if (equal == n_prev && equal == n_next) {
    update.kind  = BrushCoverageUpdateKind::Unchanged;
    update.dirty = OneTexelDirty(raster);
    return update;
  }

  if (equal == n_prev && n_next > n_prev) {
    RectI dirty{};
    for (auto i = n_prev; i < n_next; ++i) {
      dirty = UnionTexelRect(dirty, BrushStrokeOutputTexelSupport(next.strokes[i],
                                                                  next.placement_translation,
                                                                  raster, full_reference));
    }
    update.kind         = BrushCoverageUpdateKind::StampNewSamples;
    update.stroke_index = n_prev;
    update.stroke_end   = n_next;
    update.sample_begin = 0;
    update.dirty        = ClipTexelRect(dirty, raster);
    if (RectIEmpty(update.dirty)) {
      update.dirty = OneTexelDirty(raster);
    }
    return update;
  }

  if (equal + 1 == n_prev && n_prev <= n_next &&
      StrokeIdentityMatches(previous->strokes[equal], next.strokes[equal])) {
    const auto prev_samples = BrushStrokeSamples(previous->strokes[equal]);
    const auto next_samples = BrushStrokeSamples(next.strokes[equal]);
    if (SamplesArePrefix(prev_samples, next_samples) && prev_samples.size() < next_samples.size()) {
      RectI dirty = UnionNewSampleSupports(next.strokes[equal], next.placement_translation,
                                           prev_samples.size(), next_samples.size(), raster,
                                           full_reference);
      for (auto i = n_prev; i < n_next; ++i) {
        dirty = UnionTexelRect(dirty, BrushStrokeOutputTexelSupport(next.strokes[i],
                                                                    next.placement_translation,
                                                                    raster, full_reference));
      }
      update.kind         = BrushCoverageUpdateKind::StampNewSamples;
      update.stroke_index = equal;
      update.stroke_end   = n_next;
      update.sample_begin = prev_samples.size();
      update.dirty        = ClipTexelRect(dirty, raster);
      if (RectIEmpty(update.dirty)) {
        update.dirty = OneTexelDirty(raster);
      }
      return update;
    }
  }

  update.kind  = BrushCoverageUpdateKind::ReplayDirty;
  update.dirty = ChangedStrokeDirty(*previous, next, raster, full_reference);
  return update;
}

auto BrushCoverageCommandJournal::Find(const NodeId& owner_node_id, const MaskId& mask_id) const
    -> const Entry* {
  const auto found = entries_.find({owner_node_id, mask_id});
  if (found == entries_.end()) {
    return nullptr;
  }
  return &found->second;
}

void BrushCoverageCommandJournal::Store(const NodeId& owner_node_id, const MaskId& mask_id,
                                        BrushMaskSource source, Extent2D raster,
                                        Extent2D full_reference) {
  Entry entry;
  entry.source          = std::move(source);
  entry.raster          = raster;
  entry.full_reference  = full_reference;
  entries_[{owner_node_id, mask_id}] = std::move(entry);
}

void BrushCoverageCommandJournal::Clear() { entries_.clear(); }

}  // namespace alcedo
