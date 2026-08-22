//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <bit>
#include <cstdint>
#include <span>
#include <string_view>

namespace alcedo {

inline constexpr std::uint64_t kContentHashOffset = 14695981039346656037ull;
inline constexpr std::uint64_t kContentHashPrime  = 1099511628211ull;

/// Bumped when SensorDevelop pixel rules change.
inline constexpr std::uint32_t kSensorDevelopImplementationVersion = 1;
/// Bumped when GeometryResample sampling rules change.
inline constexpr std::uint32_t kGeometryImplementationVersion = 1;
/// Bumped when CameraColorPass math changes. G7R.3 replaces the current matrix path.
inline constexpr std::uint32_t kCameraColorImplementationVersion = 1;
/// Bumped when Primary Grade pixel rules change.
inline constexpr std::uint32_t kPrimaryGradeImplementationVersion = 1;
/// Bumped when DRT pixel rules change.
inline constexpr std::uint32_t kDrtImplementationVersion = 1;
/// Bumped when mask raster sampling rules change.
inline constexpr std::uint32_t kMaskImplementationVersion = 1;

/**
 * @brief Content identity of a cached GPU image result.
 *
 * Built from input values and implementation versions. Not a write counter, texture
 * address, or ResourceId.
 */
struct ContentKey {
  std::uint64_t hash = 0;

  [[nodiscard]] auto Empty() const -> bool { return hash == 0; }
};

struct ImageExtent {
  std::uint32_t width  = 0;
  std::uint32_t height = 0;
};

inline auto operator==(ImageExtent a, ImageExtent b) -> bool {
  return a.width == b.width && a.height == b.height;
}

inline auto operator!=(ImageExtent a, ImageExtent b) -> bool { return !(a == b); }

inline auto operator==(ContentKey a, ContentKey b) -> bool { return a.hash == b.hash; }
inline auto operator!=(ContentKey a, ContentKey b) -> bool { return a.hash != b.hash; }
inline auto operator<(ContentKey a, ContentKey b) -> bool { return a.hash < b.hash; }

/**
 * @brief Incremental FNV-1a mixer for ContentKey construction.
 *
 * Not thread-safe. One builder per key.
 */
class ContentHash {
 public:
  ContentHash() = default;

  auto MixU64(std::uint64_t value) -> ContentHash& {
    hash_ ^= value;
    hash_ *= kContentHashPrime;
    return *this;
  }

  auto MixU32(std::uint32_t value) -> ContentHash& { return MixU64(value); }

  auto MixI32(std::int32_t value) -> ContentHash& {
    return MixU32(static_cast<std::uint32_t>(value));
  }

  auto MixBool(bool value) -> ContentHash& { return MixU64(value ? 1ull : 0ull); }

  auto MixF32(float value) -> ContentHash& { return MixU32(std::bit_cast<std::uint32_t>(value)); }

  auto MixF64(double value) -> ContentHash& { return MixU64(std::bit_cast<std::uint64_t>(value)); }

  auto MixKey(ContentKey key) -> ContentHash& { return MixU64(key.hash); }

  auto MixBytes(std::span<const std::byte> bytes) -> ContentHash& {
    for (const auto byte : bytes) {
      MixU64(static_cast<std::uint8_t>(byte));
    }
    return *this;
  }

  auto MixText(std::string_view text) -> ContentHash& {
    MixU64(text.size());
    for (const unsigned char byte : text) {
      MixU64(byte);
    }
    return MixU64(0xFFull);
  }

  [[nodiscard]] auto Key() const -> ContentKey { return ContentKey{hash_}; }

 private:
  std::uint64_t hash_ = kContentHashOffset;
};

}  // namespace alcedo
