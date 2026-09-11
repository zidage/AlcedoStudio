//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "edit/geometry/types.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/mask/active_raster_mask.hpp"
#include "edit/mask/brush_rasterizer.hpp"
#include "edit/mask/brush_spatial_index.hpp"
#include "edit/mask/mask_id.hpp"
#include "edit/mask/mask_model.hpp"

namespace alcedo {

/**
 * @brief Workspace-retained canonical Brush rasters with regional replay.
 *
 * One entry per (Grade, Mask) keeps the rasterized R8 and the source that
 * produced it. A @ref Replay call diffs the incoming source against the entry:
 * unchanged pixels return without rasterizing, an appended or regrown stroke
 * replays only its output-texel support, and a placement move replays the union
 * of the old and new coverage bounds. Geometry changes, reordered or removed
 * strokes, and body replacements restart the affected texels through
 * @ref BrushRasterizer::ReplayRegion — never a weaker path.
 *
 * Entries own shared sample bodies through the stored source copy; retaining a
 * bounded number of entries bounds host memory. Cleared with the workspace
 * session. Thread: serial owner worker. Not thread-safe.
 */
class ParameterizedBrushReplayCache {
 public:
  /// Upper bound on retained canonical rasters; the least-recently-used entry
  /// beyond it is evicted and rasters restart on its next replay.
  static constexpr std::size_t kMaxRetainedEntries = 4;

  /**
   * @brief Canonical R8 input for ( @p owner_node_id, @p mask_id ).
   *
   * The returned pixels stay valid until the next call for the same key or
   * @ref Clear. @p dirty_rectangle names the texels that differ from the
   * previously returned raster for this key, clipped to the descriptor extent;
   * it is never empty so the upload contract holds even when only the revision
   * advanced.
   *
   * @param content_revision Live Mask revision used by the texture cache to
   *        reject stale uploads; 0 maps to 1.
   * @param session_generation Authoring-session isolation; default 1 is the
   *        live Interactive Mix slot.
   * @throws std::runtime_error when extents are empty, the algorithm version is
   *         not 1, or a stroke fails rasterization.
   */
  [[nodiscard]] auto Replay(const NodeId& owner_node_id, const MaskId& mask_id,
                            const BrushMaskSource& source, Extent2D full_reference,
                            std::uint64_t content_revision,
                            std::uint64_t session_generation = 1) -> ActiveRasterMaskInput;

  /** @brief Drop every retained raster, index, and source copy. */
  void Clear();

  [[nodiscard]] auto EntryCount() const -> std::size_t { return entries_.size(); }

 private:
  struct Entry {
    /// Canonical R8 rasterizer owning the retained pixel buffer.
    BrushRasterizer   rasterizer;
    /// Local-texel span index rebuilt whenever the source changes.
    BrushSpatialIndex index;
    /// Last replayed source; shares immutable sample bodies with the caller.
    BrushMaskSource   source{};
    /// Canonical raster extent used by @ref rasterizer.
    Extent2D          raster{};
    bool              valid = false;
    std::uint64_t     lru_tick = 0;
  };

  void EvictOverflow();

  std::map<std::pair<NodeId, MaskId>, Entry> entries_;
  std::uint64_t                              lru_clock_ = 0;
};

}  // namespace alcedo
