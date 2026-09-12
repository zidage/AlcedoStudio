//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <map>
#include <utility>

#include "edit/geometry/types.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/mask/mask_id.hpp"
#include "edit/mask/mask_model.hpp"

namespace alcedo {

/**
 * @brief How the next parameterized Brush source differs from the last stamped source.
 *
 * @c StampNewSamples means existing pixels stay; only the named sample range is
 * applied in evaluation order. @c ReplayDirty zeros @c dirty and restamps every
 * contributing dab. @c Unchanged means no pixel work.
 */
enum class BrushCoverageUpdateKind {
  Unchanged,
  StampNewSamples,
  ReplayDirty,
};

/**
 * @brief Incremental Brush raster work for one source replacement.
 *
 * @p dirty is in canonical output texels, clipped to @p raster. Stamp ranges use
 * the incoming source's stroke list.
 */
struct BrushCoverageUpdate {
  BrushCoverageUpdateKind kind         = BrushCoverageUpdateKind::ReplayDirty;
  /// First incoming stroke to stamp for @c StampNewSamples.
  std::size_t             stroke_index = 0;
  /// Exclusive end stroke index for @c StampNewSamples (incoming source).
  std::size_t             stroke_end   = 0;
  /// Sample begin on @c stroke_index only; later strokes stamp from 0.
  std::size_t             sample_begin = 0;
  RectI                   dirty{};
};

/**
 * @brief Classify @p next against the last stamped @p previous.
 *
 * @p previous null, geometry mismatch, placement change, removals, and unordered
 * sample edits produce @c ReplayDirty. A shared-prefix growth of the last stroke
 * or appended trailing strokes produce @c StampNewSamples. Equal stroke lists and
 * placement produce @c Unchanged (feather is not coverage). @c dirty is a
 * one-texel rectangle so host-upload callers still have a valid patch region.
 *
 * @throws std::invalid_argument when extents are empty.
 * @throws std::runtime_error when @p next uses an unsupported algorithm version.
 */
[[nodiscard]] auto DetectBrushCoverageUpdate(const BrushMaskSource* previous, Extent2D prev_raster,
                                             Extent2D prev_full_reference,
                                             const BrushMaskSource& next, Extent2D raster,
                                             Extent2D full_reference) -> BrushCoverageUpdate;

/**
 * @brief Last stamped parameterized Brush command per (Grade, Mask).
 *
 * Stores the source (shared sample bodies) and geometry, not pixels. Cleared with
 * the render workspace session. Thread: serial native Mask pass. Not thread-safe.
 */
class BrushCoverageCommandJournal {
 public:
  struct Entry {
    BrushMaskSource source{};
    Extent2D        raster{};
    Extent2D        full_reference{};
  };

  [[nodiscard]] auto Find(const NodeId& owner_node_id, const MaskId& mask_id) const
      -> const Entry*;
  void Store(const NodeId& owner_node_id, const MaskId& mask_id, BrushMaskSource source,
             Extent2D raster, Extent2D full_reference);
  void Clear();

 private:
  std::map<std::pair<NodeId, MaskId>, Entry> entries_;
};

}  // namespace alcedo
