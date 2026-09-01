//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "edit/geometry/types.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/mask/mask_asset.hpp"
#include "edit/mask/mask_id.hpp"

namespace alcedo {

/**
 * @brief Task-owned Brush pixels for one render. Not stored in PipelineDocument or MaskStore.
 *
 * @p pixels must remain immutable for the lifetime of the request. Duplicate
 * (owner_node_id, mask_id) entries are rejected. @p dirty_rectangle is in raster
 * texel space and must name a non-empty clipped region.
 */
struct ActiveRasterMaskInput {
  NodeId                                           owner_node_id;
  MaskId                                           mask_id;
  std::uint64_t                                    session_generation = 0;
  std::uint64_t                                    content_revision   = 0;
  MaskAssetDescriptor                              descriptor{};
  std::shared_ptr<const std::vector<std::uint8_t>> pixels;
  RectI                                            dirty_rectangle{};
};

/**
 * @brief GPU preview texture identity for one Brush authoring session.
 *
 * Persistent textures stay keyed by @ref MaskAssetKey. Active textures never share
 * that key space.
 */
struct ActiveRasterTextureKey {
  NodeId        owner_node_id;
  MaskId        mask_id;
  std::uint64_t session_generation = 0;

  [[nodiscard]] auto DebugText() const -> std::string {
    return std::string{owner_node_id.Value()} + "/" + std::string{mask_id.Value()} + "/" +
           std::to_string(session_generation);
  }

  friend auto operator==(const ActiveRasterTextureKey&, const ActiveRasterTextureKey&)
      -> bool = default;
  friend auto operator<(const ActiveRasterTextureKey& lhs, const ActiveRasterTextureKey& rhs)
      -> bool {
    if (lhs.owner_node_id != rhs.owner_node_id) return lhs.owner_node_id < rhs.owner_node_id;
    if (lhs.mask_id != rhs.mask_id) return lhs.mask_id < rhs.mask_id;
    return lhs.session_generation < rhs.session_generation;
  }
};

/**
 * @brief Clip @p rectangle to @p extent in left-closed, right-open texel space.
 *
 * @return Empty rectangle when the intersection has no samples.
 */
[[nodiscard]] auto ClipRasterDirtyRectangle(RectI rectangle, Extent2D extent) -> RectI;

/**
 * @brief Copy one clipped R8 rectangle from tightly packed host pixels.
 *
 * @pre @p rectangle is non-empty and already clipped to @p extent.
 */
[[nodiscard]] auto CopyPackedR8Rectangle(std::span<const std::uint8_t> pixels, Extent2D extent,
                                         RectI rectangle) -> std::vector<std::byte>;

/**
 * @brief Find the input for one Grade-owned Mask, or null when absent.
 */
[[nodiscard]] auto FindActiveRasterMaskInput(std::span<const ActiveRasterMaskInput> inputs,
                                             const NodeId& owner_node_id, const MaskId& mask_id)
    -> const ActiveRasterMaskInput*;

/**
 * @brief Validate request-owned fields: identity, pixels, descriptor, and dirty rectangle.
 *
 * Does not inspect the document. Duplicate (NodeId, MaskId) pairs fail.
 *
 * @throws std::invalid_argument or std::runtime_error when the request is unusable.
 */
void ValidateActiveRasterMaskFields(std::span<const ActiveRasterMaskInput> inputs);

}  // namespace alcedo
