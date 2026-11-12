// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "image/dcp_camera_profile.hpp"
#include "image/dcp_file_reader.hpp"

namespace alcedo {

/**
 * @brief Compact camera keys of an image camera, for an exact match against `UniqueCameraModel`.
 *
 * The keys are the compact camera key (lower-case letters and digits) of @p camera_model, and of
 * each profile make name of @p camera_make followed by @p camera_model. The make alias table maps
 * the first word of the make to the make names that `UniqueCameraModel` uses ("NIKON CORPORATION"
 * gives "Nikon", "OLYMPUS IMAGING CORP." gives "Olympus" and "OM Digital Solutions"). A make that
 * the table does not hold uses its own first word.
 *
 * @return Distinct non-empty keys; empty when the model is empty.
 */
auto DcpCameraMatchKeys(const std::string& camera_make, const std::string& camera_model)
    -> std::vector<std::string>;

/// Compact camera key of a `UniqueCameraModel` value.
auto DcpUniqueCameraModelKey(const std::string& unique_camera_model) -> std::string;

/**
 * @brief True when a profile with @p unique_camera_model belongs to the image camera.
 *
 * The match is exact after name normalization (plan section 2.4): the compact key of
 * @p unique_camera_model must equal one key of @ref DcpCameraMatchKeys. "Canon EOS R6" does not
 * match "Canon EOS R6 Mark II".
 */
auto DcpCameraModelMatches(const std::string& camera_make, const std::string& camera_model,
                           const std::string& unique_camera_model) -> bool;

/// Outcome of one @ref DcpProfileLibrary::Scan.
struct DcpLibraryScanResult {
  std::filesystem::path     root;                ///< The scanned folder; empty for no folder.
  std::size_t               profile_count  = 0;  ///< Profiles in the published index.
  std::size_t               camera_count   = 0;  ///< Distinct compact camera keys in the index.
  std::size_t               rejected_count = 0;  ///< `.dcp` files whose identity read failed.
  /// Operating system text of a folder error; empty when the folder was read.
  std::string               error;
  std::chrono::milliseconds elapsed{0};  ///< Wall time of the scan, folder walk included.
};

/**
 * @brief In-memory index of the DCP files of one folder, and a cache of loaded profiles.
 *
 * - @ref Scan walks the folder recursively, reads only the identity of each `.dcp` file
 *   (extension compared without case), and publishes the complete index in one step. A file
 *   whose identity read fails is counted as rejected and logged with its path and the reason.
 *   Two files with one identity keep the path that sorts first; the other is logged.
 * - @ref Match and @ref Find wait for a running scan, then read the published index.
 * - @ref Find loads the full profile on request and keeps the last @ref kLoadedProfileCapacity
 *   profiles in a least-recently-used cache. The cache key is the path; a changed file size or
 *   last write time reads the file again.
 *
 * Nothing of the index or the profiles is written to a file or a database (plan decision D4).
 *
 * Thread safety: all members are safe to call from any thread. A scan holds the index lock for
 * its whole duration, so scans run one after another and readers see the previous index only
 * before the scan starts, and the new index after it ends. The profile loader runs without a
 * lock.
 */
class DcpProfileLibrary {
 public:
  /// Reads a complete profile; must be callable from any thread and throw on a read error.
  using Loader = std::function<DcpCameraProfilePtr(const std::filesystem::path&)>;

  static constexpr std::size_t kLoadedProfileCapacity = 32;

  /// @param loader Defaults to @ref ReadDcpCameraProfile.
  /// @param capacity Loaded-profile cache size; must be at least 1.
  explicit DcpProfileLibrary(Loader      loader   = ReadDcpCameraProfile,
                             std::size_t capacity = kLoadedProfileCapacity);

  /// The process-wide library that the Settings page and the render bindings share.
  static auto        Shared() -> DcpProfileLibrary&;

  /**
   * @brief Replace the index with the DCP files below @p root.
   *
   * An empty @p root clears the index and reports no error. A folder that cannot be opened or
   * walked clears the index and reports the operating system text in
   * @ref DcpLibraryScanResult::error. The loaded-profile cache stays; its entries check their
   * files on each @ref Find.
   */
  auto               Scan(const std::filesystem::path& root) -> DcpLibraryScanResult;

  /**
   * @brief Identities of the profiles that match the camera (@ref DcpCameraModelMatches).
   * @return Sorted by profile name, then by camera model; empty when no profile matches.
   */
  [[nodiscard]] auto Match(const std::string& camera_make, const std::string& camera_model) const
      -> std::vector<DcpProfileIdentity>;

  /**
   * @brief The loaded profile of an identity.
   *
   * @p camera_model is compared by its compact camera key, @p profile_name exactly.
   * @return The profile, or null when the index holds no such identity.
   * @throws std::runtime_error when the file cannot be read, or when it now holds another
   *         identity than the index records. A failed load leaves no cache entry.
   */
  auto Find(const std::string& camera_model, const std::string& profile_name)
      -> DcpCameraProfilePtr;

  /**
   * @brief The tables of @p dcp for the DNG source with profile @p source (plan phase C6).
   *
   * The result is MergeDcpTablesWithSourceCalibration of the two. Equal inputs return one shared
   * profile while any holder keeps it alive, so all documents of one image hold one merged
   * object. Inputs are equal when the fingerprints of @p dcp and @p source and the source
   * calibration of @p source are equal. The map holds no strong reference; an expired entry is
   * computed again.
   *
   * @throws std::invalid_argument when an argument is null; rethrows merge errors.
   */
  auto SourceTables(const DcpCameraProfilePtr& dcp, const DngColorProfilePtr& source)
      -> DngColorProfilePtr;

  /// Merges since construction (map misses and expired entries).
  [[nodiscard]] auto SourceTablesMergeCount() const -> std::uint64_t;

  /// Folder of the published index.
  [[nodiscard]] auto Root() const -> std::filesystem::path;
  [[nodiscard]] auto ProfileCount() const -> std::size_t;
  [[nodiscard]] auto CameraCount() const -> std::size_t;
  /// Loader calls since construction (cache misses and changed files).
  [[nodiscard]] auto ProfileReadCount() const -> std::uint64_t;

 private:
  /// One indexed DCP file.
  struct IndexEntry {
    std::filesystem::path           path;
    std::uintmax_t                  file_size = 0;
    std::filesystem::file_time_type last_write_time{};
    DcpProfileIdentity              identity;
    std::string                     camera_key;  ///< DcpUniqueCameraModelKey of the identity.
  };
  struct FileStamp {
    std::uintmax_t                  size = 0;
    std::filesystem::file_time_type last_write_time{};
    auto                            operator==(const FileStamp&) const -> bool = default;
  };
  struct CacheEntry {
    std::wstring        key;
    FileStamp           stamp;
    DcpCameraProfilePtr profile;
  };

  static auto IdentityKey(const std::string& camera_key, const std::string& profile_name)
      -> std::string;

  Loader                                                            loader_;
  std::size_t                                                       capacity_;

  mutable std::shared_mutex                                         index_mutex_;
  std::filesystem::path                                             root_;
  std::vector<IndexEntry>                                           entries_;
  /// IdentityKey to the position in entries_.
  std::unordered_map<std::string, std::size_t>                      by_identity_;
  /// Compact camera key to positions in entries_.
  std::unordered_map<std::string, std::vector<std::size_t>>         by_camera_;

  mutable std::mutex                                                cache_mutex_;
  /// Front is the most recently used profile.
  std::list<CacheEntry>                                             cache_;
  std::unordered_map<std::wstring, std::list<CacheEntry>::iterator> cache_by_path_;
  std::uint64_t                                                     profile_reads_ = 0;

  /// DCP fingerprint, source fingerprint, and source calibration of one merge.
  using SourceTablesKey = std::tuple<std::uint64_t, std::uint64_t, std::string,
                                     std::array<double, 9>, std::array<double, 9>>;
  mutable std::mutex                                              source_tables_mutex_;
  std::map<SourceTablesKey, std::weak_ptr<const DngColorProfile>> source_tables_;
  std::uint64_t                                                   source_tables_merges_ = 0;
};

}  // namespace alcedo
