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

inline constexpr std::uint32_t kMaximumRasterMaskAxis     = 4096;
inline constexpr std::uint32_t kMaskAssetFormatVersion    = 1;
inline constexpr std::uint32_t kMaskAssetPackedR8FormatId = 1;

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

/**
 * @brief Validates dimensions and tightly packed R8 byte count.
 *
 * @param descriptor Raster extent and reference bounds.
 * @param pixels Row-major R8 samples. Size must equal width * height.
 * @throws std::invalid_argument when axes, bounds, or byte count are invalid.
 */
void ValidateMaskAssetPixels(const MaskAssetDescriptor& descriptor,
                             std::span<const std::uint8_t> pixels);

/**
 * @brief Validates key, dimensions, and tightly packed R8 byte count.
 *
 * @throws std::invalid_argument when the key is empty or pixels are invalid.
 */
void ValidateMaskAsset(const MaskAsset& asset);

/**
 * @brief Canonical bytes hashed into a content-addressed @ref MaskAssetKey.
 *
 * Layout is little-endian: format version, packed-R8 format id, width, height,
 * IEEE-754 bits of the four reference-bounds floats, pixel byte count, then
 * row-major pixels.
 *
 * @pre @ref ValidateMaskAssetPixels succeeds for the same arguments.
 */
[[nodiscard]] auto CanonicalMaskAssetBytes(const MaskAssetDescriptor& descriptor,
                                           std::span<const std::uint8_t> pixels)
    -> std::vector<std::uint8_t>;

/**
 * @brief 128-bit xxHash of @ref CanonicalMaskAssetBytes, encoded as 32 lowercase hex digits.
 *
 * @throws std::invalid_argument when @ref ValidateMaskAssetPixels fails.
 */
[[nodiscard]] auto MakeMaskAssetKey(const MaskAssetDescriptor& descriptor,
                                    std::span<const std::uint8_t> pixels) -> MaskAssetKey;

}  // namespace alcedo
