//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

#include "type/type.hpp"

namespace alcedo {

// Resolution tiers for thumbnail requests. Values are the max-edge pixel size.
// These are fixed tiers to simplify cache management and memory alignment.
enum class ThumbnailResolution : uint32_t {
  k256  = 256,
  k512  = 512,
  k1024 = 1024,
  k2048 = 2048,
};

// Composite memory cache key: element + resolution tier.
// Different resolutions of the same element are independent cache entries.
struct ThumbnailCacheKey {
  sl_element_id_t     element_id = 0;
  ThumbnailResolution resolution = ThumbnailResolution::k1024;

  bool                operator==(const ThumbnailCacheKey& other) const = default;
};

/**
 * @brief Whether a thumbnail/analysis disk write may use the queued commit label.
 *
 * Thumbnail and export pixels come from the live document at render-lock time.
 * The disk index is keyed by the commit label captured when the request was
 * queued. Skip the write when those labels differ, when an editor preview is
 * still unsettled, or when the live document is dirty. Still deliver the pixels.
 * Empty labels never write.
 */
[[nodiscard]] inline auto ThumbnailDiskCacheWriteAllowed(std::string_view queued_commit_label,
                                                          std::string_view rendered_commit_label,
                                                          bool live_has_unsettled_preview,
                                                          bool live_is_dirty) -> bool {
  if (queued_commit_label.empty() || rendered_commit_label.empty()) {
    return false;
  }
  if (queued_commit_label != rendered_commit_label) {
    return false;
  }
  if (live_has_unsettled_preview || live_is_dirty) {
    return false;
  }
  return true;
}

}  // namespace alcedo

template <>
struct std::hash<alcedo::ThumbnailCacheKey> {
  size_t operator()(const alcedo::ThumbnailCacheKey& key) const noexcept {
    const auto h1 = std::hash<std::uint32_t>{}(key.element_id);
    const auto h2 = std::hash<std::uint32_t>{}(static_cast<std::uint32_t>(key.resolution));
    return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
  }
};
