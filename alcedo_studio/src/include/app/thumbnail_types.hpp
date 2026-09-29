//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <functional>

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

}  // namespace alcedo

template <>
struct std::hash<alcedo::ThumbnailCacheKey> {
  size_t operator()(const alcedo::ThumbnailCacheKey& key) const noexcept {
    const auto h1 = std::hash<std::uint32_t>{}(key.element_id);
    const auto h2 = std::hash<std::uint32_t>{}(static_cast<std::uint32_t>(key.resolution));
    return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
  }
};
