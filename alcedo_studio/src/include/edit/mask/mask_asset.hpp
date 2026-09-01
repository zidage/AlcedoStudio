//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "edit/geometry/types.hpp"

namespace alcedo {

inline constexpr std::uint32_t kMaximumRasterMaskAxis = 4096;

/** @brief Stable serialized identifier for one persistent raster mask. */
class MaskAssetKey {
 public:
  MaskAssetKey() = default;
  explicit MaskAssetKey(std::string value) : value_(std::move(value)) {}

  [[nodiscard]] auto Value() const -> std::string_view { return value_; }
  [[nodiscard]] auto Empty() const -> bool { return value_.empty(); }

  friend auto        operator==(const MaskAssetKey&, const MaskAssetKey&) -> bool = default;
  friend auto        operator<(const MaskAssetKey& lhs, const MaskAssetKey& rhs) -> bool {
    return lhs.value_ < rhs.value_;
  }

 private:
  std::string value_;
};

/** @brief GPU-free metadata stored with tightly packed R8 pixels. */
struct MaskAssetDescriptor {
  Extent2D       extent{};
  NormalizedRect reference_bounds{};
};

inline auto operator==(const MaskAssetDescriptor& a, const MaskAssetDescriptor& b) -> bool {
  return a.extent == b.extent && a.reference_bounds.x == b.reference_bounds.x &&
         a.reference_bounds.y == b.reference_bounds.y && a.reference_bounds.w == b.reference_bounds.w &&
         a.reference_bounds.h == b.reference_bounds.h;
}

inline auto operator!=(const MaskAssetDescriptor& a, const MaskAssetDescriptor& b) -> bool {
  return !(a == b);
}

/** @brief Persistent R8 raster mask value. */
struct MaskAsset {
  MaskAssetKey              key;
  MaskAssetDescriptor       descriptor;
  std::vector<std::uint8_t> pixels;

  [[nodiscard]] auto        ByteSize() const -> std::size_t { return pixels.size(); }
};

/** @brief Validates key, dimensions, and tightly packed R8 byte count. */
void ValidateMaskAsset(const MaskAsset& asset);

}  // namespace alcedo
