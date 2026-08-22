//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/mask/mask_store.hpp"

#include <array>
#include <atomic>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace alcedo {
namespace {

constexpr std::array<char, 8> kMagic   = {'A', 'L', 'C', 'R', '8', 'M', 'S', 'K'};
constexpr std::uint32_t       kVersion = 1;
std::atomic<std::uint64_t>    g_temp_sequence{0};

struct FileHeader {
  char          magic[8];
  std::uint32_t version;
  std::uint32_t width;
  std::uint32_t height;
  float         bounds[4];
  std::uint32_t key_bytes;
  std::uint32_t pixel_bytes;
};
static_assert(sizeof(FileHeader) == 44);

auto SafeFileName(const MaskAssetKey& key) -> std::string {
  if (key.Empty()) throw std::invalid_argument("MaskAssetKey must not be empty");
  constexpr char digits[] = "0123456789abcdef";
  std::string    result;
  result.reserve(key.Value().size() * 2 + 7);
  for (const unsigned char c : key.Value()) {
    result.push_back(digits[c >> 4]);
    result.push_back(digits[c & 0x0f]);
  }
  return result + ".r8mask";
}

void ReplaceCompleteFile(const std::filesystem::path& source,
                         const std::filesystem::path& destination) {
#ifdef _WIN32
  if (!::MoveFileExW(source.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(),
                            "MaskStore atomic replacement failed");
  }
#else
  std::filesystem::rename(source, destination);
#endif
}

}  // namespace

void ValidateMaskAsset(const MaskAsset& asset) {
  if (asset.key.Empty()) throw std::invalid_argument("MaskAsset key must not be empty");
  const auto extent = asset.descriptor.extent;
  if (extent.Empty() || extent.width > kMaximumRasterMaskAxis ||
      extent.height > kMaximumRasterMaskAxis) {
    throw std::invalid_argument("Raster mask axes must be in [1, 4096]");
  }
  const auto required = static_cast<std::size_t>(extent.width) * extent.height;
  if (asset.pixels.size() != required) {
    throw std::invalid_argument("Raster mask must contain tightly packed R8 pixels");
  }
}

MaskStore::MaskStore(std::filesystem::path root, std::size_t host_cache_budget_bytes)
    : root_(std::move(root)), host_cache_budget_bytes_(host_cache_budget_bytes) {
  if (root_.empty()) throw std::invalid_argument("MaskStore root must not be empty");
}

auto MaskStore::PathFor(const MaskAssetKey& key) const -> std::filesystem::path {
  return root_ / SafeFileName(key);
}

void MaskStore::SetHostCacheBudget(std::size_t bytes) {
  host_cache_budget_bytes_ = bytes;
  EvictHostCache();
}

void MaskStore::Save(const MaskAsset& asset) {
  ValidateMaskAsset(asset);
  std::filesystem::create_directories(root_);
  const auto destination = PathFor(asset.key);
  auto       temporary   = destination;
  temporary += ".tmp." + std::to_string(++g_temp_sequence);
  try {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) throw std::runtime_error("MaskStore could not open temporary file");
    FileHeader header{};
    std::memcpy(header.magic, kMagic.data(), kMagic.size());
    header.version     = kVersion;
    header.width       = asset.descriptor.extent.width;
    header.height      = asset.descriptor.extent.height;
    header.bounds[0]   = asset.descriptor.reference_bounds.x;
    header.bounds[1]   = asset.descriptor.reference_bounds.y;
    header.bounds[2]   = asset.descriptor.reference_bounds.w;
    header.bounds[3]   = asset.descriptor.reference_bounds.h;
    header.key_bytes   = static_cast<std::uint32_t>(asset.key.Value().size());
    header.pixel_bytes = static_cast<std::uint32_t>(asset.pixels.size());
    stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
    stream.write(asset.key.Value().data(), static_cast<std::streamsize>(header.key_bytes));
    stream.write(reinterpret_cast<const char*>(asset.pixels.data()),
                 static_cast<std::streamsize>(asset.pixels.size()));
    stream.flush();
    if (!stream) throw std::runtime_error("MaskStore temporary write did not complete");
    stream.close();
    ReplaceCompleteFile(temporary, destination);
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    throw;
  }
  StoreInCache(std::make_shared<const MaskAsset>(asset));
}

auto MaskStore::Load(const MaskAssetKey& key) -> std::shared_ptr<const MaskAsset> {
  if (const auto found = cache_.find(key); found != cache_.end()) {
    lru_.splice(lru_.begin(), lru_, found->second.lru);
    return found->second.asset;
  }
  std::ifstream stream(PathFor(key), std::ios::binary);
  if (!stream) throw std::runtime_error("MaskStore asset does not exist");
  FileHeader header{};
  stream.read(reinterpret_cast<char*>(&header), sizeof(header));
  if (!stream || std::memcmp(header.magic, kMagic.data(), kMagic.size()) != 0 ||
      header.version != kVersion || header.key_bytes == 0 || header.key_bytes > 4096) {
    throw std::runtime_error("MaskStore file header is invalid");
  }
  auto        asset = std::make_shared<MaskAsset>();
  std::string stored_key(header.key_bytes, '\0');
  stream.read(stored_key.data(), static_cast<std::streamsize>(stored_key.size()));
  asset->key                         = MaskAssetKey{std::move(stored_key)};
  asset->descriptor.extent           = {header.width, header.height};
  asset->descriptor.reference_bounds = {header.bounds[0], header.bounds[1], header.bounds[2],
                                        header.bounds[3]};
  asset->pixels.resize(static_cast<std::size_t>(header.pixel_bytes));
  stream.read(reinterpret_cast<char*>(asset->pixels.data()),
              static_cast<std::streamsize>(asset->pixels.size()));
  if (!stream || asset->key != key) throw std::runtime_error("MaskStore file is incomplete");
  ValidateMaskAsset(*asset);
  StoreInCache(asset);
  return asset;
}

void MaskStore::StoreInCache(std::shared_ptr<const MaskAsset> asset) {
  const auto key = asset->key;
  if (const auto found = cache_.find(key); found != cache_.end()) {
    cache_bytes_ -= found->second.asset->ByteSize();
    lru_.erase(found->second.lru);
    cache_.erase(found);
  }
  lru_.push_front(key);
  cache_bytes_ += asset->ByteSize();
  cache_.emplace(key, CacheEntry{std::move(asset), lru_.begin()});
  EvictHostCache();
}

void MaskStore::EvictHostCache() {
  if (host_cache_budget_bytes_ == 0) return;
  while (cache_bytes_ > host_cache_budget_bytes_ && !lru_.empty()) {
    const auto key = lru_.back();
    const auto it  = cache_.find(key);
    cache_bytes_ -= it->second.asset->ByteSize();
    cache_.erase(it);
    lru_.pop_back();
  }
}

}  // namespace alcedo
