// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "image/dng_color_profile.hpp"

namespace alcedo {

/**
 * @brief Process-wide cache of DNG color profiles read from source files.
 *
 * Project data stores only a profile fingerprint (see DngColorProfileRef). The pipeline service
 * binds the profile tables through this cache before a loaded document renders.
 *
 * - Key: the source file identity, which is the normalized path plus the file size and the last
 *   write time. A changed size or write time makes the entry stale, and the next @ref Load reads
 *   the file again.
 * - Capacity: least-recently-used eviction at @ref kDefaultCapacity entries. An evicted entry is
 *   read from its source file again on the next @ref Load.
 * - Sharing: loads that give the same fingerprint return one shared DngColorProfilePtr while any
 *   holder keeps it alive, so many files from one camera use one profile in memory.
 *
 * Thread safety: all members are safe to call from any thread. The loader runs without the cache
 * lock; two threads that miss the same file at the same time can both read it, and both get the
 * shared profile.
 */
class DngColorProfileCache {
 public:
  /// Reads the profile from a source file. Returns null when the file has no DNG profile.
  using Loader = std::function<DngColorProfilePtr(const std::filesystem::path&)>;

  static constexpr std::size_t kDefaultCapacity = 100;

  /// @param loader Reads a profile; must be callable from any thread.
  /// @param capacity Maximum number of cached files; must be at least 1.
  explicit DngColorProfileCache(Loader loader, std::size_t capacity = kDefaultCapacity);

  /// The process-wide cache. Its loader is MetadataExtractor::ReadDngColorProfileFromSource.
  static auto        Shared() -> DngColorProfileCache&;

  /**
   * @brief Profile of @p source, from the cache or read from the file.
   *
   * @return The profile, or null when the file has no DNG profile.
   * @throws std::runtime_error when the file is missing; rethrows loader errors. A failed load
   *         leaves no cache entry.
   */
  auto               Load(const std::filesystem::path& source) -> DngColorProfilePtr;

  /// Number of cached files.
  [[nodiscard]] auto Size() const -> std::size_t;

  /// Number of loader calls since construction (cache misses and stale entries).
  [[nodiscard]] auto LoaderCallCount() const -> std::uint64_t;

 private:
  struct FileIdentity {
    std::uintmax_t                  size_ = 0;
    std::filesystem::file_time_type last_write_time_{};
    auto                            operator==(const FileIdentity&) const -> bool = default;
  };
  struct Entry {
    std::wstring       key_;
    FileIdentity       identity_;
    DngColorProfilePtr profile_;
  };

  static auto        NormalizedKey(const std::filesystem::path& source) -> std::wstring;
  static auto        ReadIdentity(const std::filesystem::path& source) -> FileIdentity;
  auto               ShareByFingerprintLocked(DngColorProfilePtr profile) -> DngColorProfilePtr;

  Loader             loader_;
  std::size_t        capacity_;
  mutable std::mutex mutex_;
  /// Front is the most recently used entry.
  std::list<Entry>   entries_;
  std::unordered_map<std::wstring, std::list<Entry>::iterator>            by_key_;
  std::unordered_map<std::uint64_t, std::weak_ptr<const DngColorProfile>> by_fingerprint_;
  std::uint64_t                                                           loader_calls_ = 0;
};

}  // namespace alcedo
