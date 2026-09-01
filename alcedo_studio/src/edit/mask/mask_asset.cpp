//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/mask/mask_asset.hpp"

#include <xxhash.h>

#include <cmath>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace alcedo {
namespace {

void AppendU32Le(std::vector<std::uint8_t>& out, std::uint32_t value) {
  out.push_back(static_cast<std::uint8_t>(value));
  out.push_back(static_cast<std::uint8_t>(value >> 8));
  out.push_back(static_cast<std::uint8_t>(value >> 16));
  out.push_back(static_cast<std::uint8_t>(value >> 24));
}

void AppendU64Le(std::vector<std::uint8_t>& out, std::uint64_t value) {
  AppendU32Le(out, static_cast<std::uint32_t>(value));
  AppendU32Le(out, static_cast<std::uint32_t>(value >> 32));
}

void AppendF32BitsLe(std::vector<std::uint8_t>& out, float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  AppendU32Le(out, bits);
}

auto Hex128(const XXH128_hash_t& hash) -> std::string {
  std::ostringstream text;
  text << std::hex << std::setfill('0') << std::setw(16) << hash.high64 << std::setw(16)
       << hash.low64;
  return text.str();
}

}  // namespace

void ValidateMaskAssetPixels(const MaskAssetDescriptor& descriptor,
                             std::span<const std::uint8_t> pixels) {
  const auto extent = descriptor.extent;
  if (extent.Empty() || extent.width > kMaximumRasterMaskAxis ||
      extent.height > kMaximumRasterMaskAxis) {
    throw std::invalid_argument("Raster mask axes must be in [1, 4096]");
  }
  if (!std::isfinite(descriptor.reference_bounds.x) ||
      !std::isfinite(descriptor.reference_bounds.y) ||
      !std::isfinite(descriptor.reference_bounds.w) ||
      !std::isfinite(descriptor.reference_bounds.h)) {
    throw std::invalid_argument("Raster mask reference bounds must be finite");
  }
  const auto required =
      static_cast<std::size_t>(extent.width) * static_cast<std::size_t>(extent.height);
  if (pixels.size() != required) {
    throw std::invalid_argument("Raster mask must contain tightly packed R8 pixels");
  }
}

void ValidateMaskAsset(const MaskAsset& asset) {
  if (asset.key.Empty()) throw std::invalid_argument("MaskAsset key must not be empty");
  ValidateMaskAssetPixels(asset.descriptor, asset.pixels);
}

auto CanonicalMaskAssetBytes(const MaskAssetDescriptor& descriptor,
                             std::span<const std::uint8_t> pixels) -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(40 + pixels.size());
  AppendU32Le(bytes, kMaskAssetFormatVersion);
  AppendU32Le(bytes, kMaskAssetPackedR8FormatId);
  AppendU32Le(bytes, descriptor.extent.width);
  AppendU32Le(bytes, descriptor.extent.height);
  AppendF32BitsLe(bytes, descriptor.reference_bounds.x);
  AppendF32BitsLe(bytes, descriptor.reference_bounds.y);
  AppendF32BitsLe(bytes, descriptor.reference_bounds.w);
  AppendF32BitsLe(bytes, descriptor.reference_bounds.h);
  AppendU64Le(bytes, static_cast<std::uint64_t>(pixels.size()));
  bytes.insert(bytes.end(), pixels.begin(), pixels.end());
  return bytes;
}

auto MakeMaskAssetKey(const MaskAssetDescriptor& descriptor, std::span<const std::uint8_t> pixels)
    -> MaskAssetKey {
  ValidateMaskAssetPixels(descriptor, pixels);
  const auto          canonical = CanonicalMaskAssetBytes(descriptor, pixels);
  const XXH128_hash_t digest    = XXH3_128bits(canonical.data(), canonical.size());
  return MaskAssetKey{Hex128(digest)};
}

}  // namespace alcedo
