// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#include "image/dng_color_profile_cache.hpp"

#include <iterator>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "image/metadata_extractor.hpp"

namespace alcedo {

DngColorProfileCache::DngColorProfileCache(Loader loader, std::size_t capacity)
    : loader_(std::move(loader)), capacity_(capacity) {
  if (!loader_) {
    throw std::invalid_argument("DngColorProfileCache: loader is required");
  }
  if (capacity_ == 0) {
    throw std::invalid_argument("DngColorProfileCache: capacity must be at least 1");
  }
}

auto DngColorProfileCache::Shared() -> DngColorProfileCache& {
  static DngColorProfileCache cache(
      [](const std::filesystem::path& source) {
        return MetadataExtractor::ReadDngColorProfileFromSource(source);
      },
      kDefaultCapacity);
  return cache;
}

auto DngColorProfileCache::NormalizedKey(const std::filesystem::path& source) -> std::wstring {
  std::error_code ec;
  auto            normalized = std::filesystem::weakly_canonical(source, ec);
  if (ec) {
    normalized = std::filesystem::absolute(source, ec);
    if (ec) {
      normalized = source;
    }
  }
  return normalized.lexically_normal().generic_wstring();
}

auto DngColorProfileCache::ReadIdentity(const std::filesystem::path& source) -> FileIdentity {
  std::error_code ec;
  FileIdentity    identity;
  identity.size_ = std::filesystem::file_size(source, ec);
  if (!ec) {
    identity.last_write_time_ = std::filesystem::last_write_time(source, ec);
  }
  if (ec) {
    throw std::runtime_error("DngColorProfileCache: source file is unavailable: " +
                             source.string() + " (" + ec.message() + ")");
  }
  return identity;
}

auto DngColorProfileCache::ShareByFingerprintLocked(DngColorProfilePtr profile)
    -> DngColorProfilePtr {
  if (!profile) {
    return profile;
  }
  if (const auto it = by_fingerprint_.find(profile->fingerprint); it != by_fingerprint_.end()) {
    if (auto existing = it->second.lock()) {
      return existing;
    }
  }
  // Drop fingerprints that no holder keeps alive, so the map stays as small as the live set.
  for (auto it = by_fingerprint_.begin(); it != by_fingerprint_.end();) {
    it = it->second.expired() ? by_fingerprint_.erase(it) : std::next(it);
  }
  by_fingerprint_[profile->fingerprint] = profile;
  return profile;
}

auto DngColorProfileCache::Load(const std::filesystem::path& source) -> DngColorProfilePtr {
  const auto key      = NormalizedKey(source);
  const auto identity = ReadIdentity(source);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (const auto it = by_key_.find(key); it != by_key_.end()) {
      if (it->second->identity_ == identity) {
        entries_.splice(entries_.begin(), entries_, it->second);
        return it->second->profile_;
      }
      entries_.erase(it->second);
      by_key_.erase(it);
    }
    ++loader_calls_;
  }

  // Read the file without the lock; a slow EXIF read must not block loads of other files.
  auto                        profile = loader_(source);

  std::lock_guard<std::mutex> lock(mutex_);
  profile = ShareByFingerprintLocked(std::move(profile));
  if (const auto it = by_key_.find(key); it != by_key_.end()) {
    // Another thread loaded the same file meanwhile; keep this newer read.
    entries_.erase(it->second);
    by_key_.erase(it);
  }
  entries_.push_front(Entry{key, identity, profile});
  by_key_[key] = entries_.begin();
  while (entries_.size() > capacity_) {
    by_key_.erase(entries_.back().key_);
    entries_.pop_back();
  }
  return profile;
}

auto DngColorProfileCache::Size() const -> std::size_t {
  std::lock_guard<std::mutex> lock(mutex_);
  return entries_.size();
}

auto DngColorProfileCache::LoaderCallCount() const -> std::uint64_t {
  std::lock_guard<std::mutex> lock(mutex_);
  return loader_calls_;
}

}  // namespace alcedo
